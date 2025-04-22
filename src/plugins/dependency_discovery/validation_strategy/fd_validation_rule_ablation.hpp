#pragma once

#include "abstract_dependency_validation_rule.hpp"

namespace hyrise {

class FdValidationRuleAblation : public AbstractDependencyValidationRule {
 public:
  FdValidationRuleAblation();

  void apply_ablation_level(const AblationLevel) final;

 protected:
  ValidationResult _on_validate(const AbstractDependencyCandidate& candidate) const override;
};

}  // namespace hyrise
