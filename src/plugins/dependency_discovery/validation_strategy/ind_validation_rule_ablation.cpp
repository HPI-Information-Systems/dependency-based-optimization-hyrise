#include "ind_validation_rule_ablation.hpp"

#include "dependency_discovery/validation_strategy/validation_utils.hpp"
#include "hyrise.hpp"
#include "resolve_type.hpp"
#include "storage/segment_iterate.hpp"
#include "storage/table.hpp"
#include "utils/format_duration.hpp"
#include "utils/print_utils.hpp"
#include "utils/timer.hpp"

namespace hyrise {

template <typename T>
ValidationSet<T> IndValidationRuleAblation::_collect_values(const std::shared_ptr<const Table>& table,
                                                            const ColumnID column_id) const {
  auto distinct_values = ValidationSet<T>(table->row_count());
  const auto chunk_count = table->chunk_count();

  for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
    const auto& chunk = table->get_chunk(chunk_id);
    if (!chunk) {
      continue;
    }

    const auto& segment = chunk->get_segment(column_id);
    if (!_skip_bulk_insert) {
      if (const auto& value_segment = std::dynamic_pointer_cast<ValueSegment<T>>(segment)) {
        // Directly insert all values.
        const auto& values = value_segment->values();
        distinct_values.insert(values.cbegin(), values.cend());
        continue;
      } else if (const auto& dictionary_segment = std::dynamic_pointer_cast<DictionarySegment<T>>(segment)) {
        // Directly insert dictionary entries.
        const auto& dictionary = dictionary_segment->dictionary();
        distinct_values.insert(dictionary->cbegin(), dictionary->cend());
        continue;
      }
    }

    // Fallback: Iterate the whole segment and decode its values.
    segment_iterate<T>(*segment, [&](const auto& position) {
      if (!position.is_null()) {
        distinct_values.insert(position.value());
      }
    });
  }

  return distinct_values;
}

template <typename T>
ValidationStatus IndValidationRuleAblation::_perform_set_based_inclusion_check(
    const std::shared_ptr<Table>& including_table, const ColumnID including_column_id,
    const std::shared_ptr<Table>& included_table, const ColumnID included_column_id,
    std::unordered_map<std::shared_ptr<Table>, std::shared_ptr<AbstractTableConstraint>>& constraints,
    const std::optional<std::pair<T, T>>& including_min_max,
    const std::optional<std::pair<T, T>>& included_min_max) const {
  // auto timer = Timer{};
  const auto including_values = _collect_values<T>(including_table, including_column_id);
  // const auto t1 = timer.lap();
  // std::cout << " build " << format_duration(t1) << "  " << including_values.size() << "\n";

  if constexpr (std::is_integral_v<T>) {
    if (!_skip_continuousness) {
      Assert(!including_values.empty(), "Empty tables not considered.");
      if (including_min_max) {
        const auto domain = static_cast<size_t>(including_min_max->second - including_min_max->first);
        if (domain == including_table->row_count() - 1) {
          constraints[including_table] =
              std::make_shared<TableKeyConstraint>(std::set<ColumnID>{including_column_id}, KeyConstraintType::UNIQUE);
        }

        // Skip probing if primary key continuous.
        Assert(included_min_max, "Could not obtain min/max values.");
        if (domain == including_values.size() - 1 && including_min_max->first <= included_min_max->first &&
            including_min_max->second >= included_min_max->second) {
          return ValidationStatus::Valid;
        }
      } else {
        PerformanceWarning("Could not obtain min/max values.");
      }
    }
  }

  auto status = ValidationStatus::Valid;
  const auto chunk_count = included_table->chunk_count();
  for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
    if (status == ValidationStatus::Invalid) {
      break;
    }
    const auto& chunk = included_table->get_chunk(chunk_id);
    if (!chunk) {
      continue;
    }

    const auto& segment = chunk->get_segment(included_column_id);
    auto inserted = false;
    if (!_skip_bulk_insert) {
      if (const auto& dictionary_segment = std::dynamic_pointer_cast<DictionarySegment<T>>(segment)) {
        for (const auto& value : *dictionary_segment->dictionary()) {
          if (!including_values.contains(value)) {
            return ValidationStatus::Invalid;
          }
        }
        inserted = true;
      }
    }
    if (!inserted) {
      segment_with_iterators<T>(*segment, [&](auto it, const auto end) {
        while (it != end) {
          if (!it->is_null() && !including_values.contains(it->value())) {
            status = ValidationStatus::Invalid;
            return;
          }
          ++it;
        }
      });
    }
  }
  // const auto t2 = timer.lap();
  // std::cout << " probe " << format_duration(t2) << "\n" << " sum " << format_duration(t1 + t2) << "\n";

  return status;
}

IndValidationRuleAblation::IndValidationRuleAblation() : AbstractDependencyValidationRule{DependencyType::Inclusion} {}

ValidationResult IndValidationRuleAblation::_on_validate(const AbstractDependencyCandidate& candidate) const {
  const auto& ind_candidate = static_cast<const IndCandidate&>(candidate);
  // std::cout << candidate << "\n";

  const auto& included_table = Hyrise::get().storage_manager.get_table(ind_candidate.foreign_key_table);
  const auto included_column_id = ind_candidate.foreign_key_column_id;

  const auto& including_table = Hyrise::get().storage_manager.get_table(ind_candidate.primary_key_table);
  const auto including_column_id = ind_candidate.primary_key_column_id;

  if (including_table->column_data_type(including_column_id) != included_table->column_data_type(included_column_id)) {
    return ValidationResult{ValidationStatus::Invalid};
  }

  auto result = ValidationResult{ValidationStatus::Uncertain};
  resolve_data_type(included_table->column_data_type(included_column_id), [&](const auto data_type_t) {
    using ColumnDataType = typename decltype(data_type_t)::type;

    const auto included_min_max =
        !_skip_min_max ? ValidationUtils<ColumnDataType>::get_column_min_max_value(included_table, included_column_id)
                       : std::nullopt;

    if (!included_min_max) {
      // PerformanceWarning("Could not obtain min/max values.");
      result.status = _perform_set_based_inclusion_check<ColumnDataType>(
          including_table, including_column_id, included_table, included_column_id, result.constraints, std::nullopt,
          std::nullopt);
      return;
    }

    auto including_min_max = std::optional<std::pair<ColumnDataType, ColumnDataType>>{};

    if constexpr (std::is_integral_v<ColumnDataType>) {
      auto including_unique_by_ucc = false;
      auto including_continuous = false;

      const auto& key_constraints = including_table->soft_key_constraints();
      if (!_skip_uniqueness) {
        for (const auto& key_constraint : key_constraints) {
          // Already checked that min/max values match. If unique, then IND must be valid.
          if (key_constraint.columns().size() != 1 || *key_constraint.columns().cbegin() != including_column_id) {
            continue;
          }
          including_unique_by_ucc = true;
          including_min_max =
              ValidationUtils<ColumnDataType>::get_column_min_max_value(including_table, including_column_id);
          if (!including_min_max) {
            result.status = _perform_set_based_inclusion_check<ColumnDataType>(
                including_table, including_column_id, included_table, included_column_id, result.constraints,
                including_min_max, included_min_max);
            return;
          }

          const auto min = including_min_max->first;
          const auto max = including_min_max->second;
          if (min > included_min_max->first || max < included_min_max->second) {
            result.status = ValidationStatus::Invalid;
            return;
          }

          const auto domain = max - min;
          including_continuous = static_cast<uint64_t>(domain) == including_table->row_count() - 1;
        }
      }
      including_continuous = _skip_continuousness ? false : including_continuous;

      auto including_unique_by_statistics = false;
      if (!including_unique_by_ucc) {
        const auto& including_statistics =
            ValidationUtils<ColumnDataType>::collect_column_statistics(including_table, including_column_id);
        if (including_statistics.min && including_statistics.max) {
          const auto min = *including_statistics.min;
          const auto max = *including_statistics.max;
          including_min_max = std::make_pair(min, max);
          if (min > included_min_max->first || max < included_min_max->second) {
            result.status = ValidationStatus::Invalid;
            return;
          }

          including_unique_by_statistics =
              !_skip_uniqueness && including_statistics.all_segments_unique && including_statistics.segments_disjoint;
          including_continuous = _skip_continuousness ? false : including_statistics.segments_continuous;
          if (including_unique_by_statistics) {
            result.constraints[including_table] = std::make_shared<TableKeyConstraint>(
                std::set<ColumnID>{including_column_id}, KeyConstraintType::UNIQUE);
          }
        }
      }

      if ((including_unique_by_ucc || including_unique_by_statistics) && including_continuous) {
        result.status = ValidationStatus::Valid;
        return;
      }
    } else if (!_skip_min_max) {
      including_min_max =
          ValidationUtils<ColumnDataType>::get_column_min_max_value(including_table, including_column_id);
      if (including_min_max && (including_min_max->first > included_min_max->first ||
                                including_min_max->second < included_min_max->second)) {
        result.status = ValidationStatus::Invalid;
        return;
      }
    }

    // auto timer = Timer{};
    result.status = _perform_set_based_inclusion_check<ColumnDataType>(
        including_table, including_column_id, included_table, included_column_id, result.constraints, including_min_max,
        included_min_max);

    // std::cout << "overall " << timer.lap_formatted() << "\n";
  });

  if (result.status == ValidationStatus::Valid) {
    result.constraints[included_table] = _constraint_from_candidate(candidate);
  }

  return result;
}

void IndValidationRuleAblation::apply_ablation_level(const AblationLevel level) {
  // CandidateDependence, IndMinMax, IndProbeDictionary, IndUniqueness, IndContinuousness, OdSampling, OdIndex, UccBulkInsert, UccDictionary, UccIndex
  _skip_min_max = level < AblationLevel::IndMinMax;
  _skip_bulk_insert = level < AblationLevel::IndProbeDictionary;
  _skip_uniqueness = level < AblationLevel::IndUniqueness;
  _skip_continuousness = level < AblationLevel::IndContinuousness;
}

}  // namespace hyrise
