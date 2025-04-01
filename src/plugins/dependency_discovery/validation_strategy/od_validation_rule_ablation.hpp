#pragma once

#include <unordered_set>
#include <utility>

#include "abstract_dependency_validation_rule.hpp"
#include "od_validation_rule.hpp"

namespace hyrise {

class Table;

class OdValidationRuleAblation : public AbstractDependencyValidationRule {
 public:
  OdValidationRuleAblation();

  void apply_ablation_level(const AblationLevel) final;

  constexpr static uint64_t SAMPLE_SIZE{100};
  constexpr static uint64_t MIN_SIZE_FOR_RANDOM_SAMPLE{SAMPLE_SIZE * 2};

 protected:
  ValidationResult _on_validate(const AbstractDependencyCandidate& candidate) const override;

 private:
  bool _skip_sampling{false};
  bool _skip_index{false};
};

}  // namespace hyrise
