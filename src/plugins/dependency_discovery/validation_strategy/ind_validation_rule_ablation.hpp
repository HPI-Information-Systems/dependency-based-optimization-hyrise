#pragma once

#include "abstract_dependency_validation_rule.hpp"

namespace hyrise {

class Table;

class IndValidationRuleAblation : public AbstractDependencyValidationRule {
 public:
  IndValidationRuleAblation();

  void apply_ablation_level(const AblationLevel level) final;

 protected:
  ValidationResult _on_validate(const AbstractDependencyCandidate& candidate) const override;

  template <typename T>
  ValidationSet<T> _collect_values(const std::shared_ptr<const Table>& table, const ColumnID column_id) const;

  template <typename T>
  ValidationStatus _perform_set_based_inclusion_check(
      const std::shared_ptr<Table>& including_table, const ColumnID including_column_id,
      const std::shared_ptr<Table>& included_table, const ColumnID included_column_id,
      std::unordered_map<std::shared_ptr<Table>, std::shared_ptr<AbstractTableConstraint>>& constraints,
      const std::optional<std::pair<T, T>>& including_min_max,
      const std::optional<std::pair<T, T>>& included_min_max) const;

  bool _skip_min_max{false};
  bool _skip_bulk_insert{false};
  bool _skip_uniqueness{false};
  bool _skip_continuousness{false};
};

}  // namespace hyrise
