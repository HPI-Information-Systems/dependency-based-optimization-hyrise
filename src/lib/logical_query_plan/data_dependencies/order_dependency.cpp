#include "order_dependency.hpp"

#include "expression/expression_functional.hpp"
#include "expression/expression_utils.hpp"
#include "expression/lqp_column_expression.hpp"
#include "logical_query_plan/abstract_lqp_node.hpp"

namespace hyrise {

using namespace expression_functional;  // NOLINT(build/namespaces)

OrderDependency::OrderDependency(const std::vector<std::shared_ptr<AbstractExpression>>& init_ordering_expressions,
                                 const std::vector<std::shared_ptr<AbstractExpression>>& init_ordered_expessions)
    : ordering_expressions{init_ordering_expressions}, ordered_expressions{init_ordered_expessions} {
  Assert(!ordering_expressions.empty() && !ordered_expressions.empty(), "OrderDependency cannot be empty.");
}

bool OrderDependency::operator==(const OrderDependency& rhs) const {
  const auto ordering_expression_count = ordering_expressions.size();
  const auto ordered_expression_count = ordered_expressions.size();
  if (ordering_expression_count != rhs.ordering_expressions.size() ||
      ordered_expression_count != rhs.ordered_expressions.size()) {
    return false;
  }

  for (auto expression_idx = size_t{0}; expression_idx < ordering_expression_count; ++expression_idx) {
    if (*ordering_expressions[expression_idx] != *rhs.ordering_expressions[expression_idx]) {
      return false;
    }
  }

  for (auto expression_idx = size_t{0}; expression_idx < ordered_expression_count; ++expression_idx) {
    if (*ordered_expressions[expression_idx] != *rhs.ordered_expressions[expression_idx]) {
      return false;
    }
  }

  return true;
}

bool OrderDependency::operator!=(const OrderDependency& rhs) const {
  return !(rhs == *this);
}

size_t OrderDependency::hash() const {
  auto hash = boost::hash_value(ordering_expressions.size());
  for (const auto& expression : ordering_expressions) {
    boost::hash_combine(hash, expression->hash());
  }

  boost::hash_combine(hash, ordered_expressions.size());
  for (const auto& expression : ordered_expressions) {
    boost::hash_combine(hash, expression->hash());
  }

  return hash;
}

std::ostream& operator<<(std::ostream& stream, const OrderDependency& od) {
  stream << "[";
  stream << od.ordering_expressions[0]->as_column_name();
  for (auto expression_idx = size_t{1}; expression_idx < od.ordering_expressions.size(); ++expression_idx) {
    stream << ", " << od.ordering_expressions[expression_idx]->as_column_name();
  }
  stream << "] |-> [";
  stream << od.ordered_expressions[0]->as_column_name();
  for (auto expression_idx = size_t{1}; expression_idx < od.ordered_expressions.size(); ++expression_idx) {
    stream << ", " << od.ordered_expressions[expression_idx]->as_column_name();
  }
  stream << "]";
  return stream;
}

void build_transitive_od_closure(OrderDependencies& order_dependencies) {
  // Usually, we do not expect to have many ODs per table with even more transitive relationships. Thus, we chose a
  // simple implementation to build the closure.
  while (true) {
    auto transitive_ods = std::vector<OrderDependency>{};
    for (const auto& od : order_dependencies) {
      const auto& ordered_expressions = od.ordered_expressions;
      for (const auto& candidate_od : order_dependencies) {
        // Given od [a] |-> [b, c], check if candidate_od looks like [b] |-> [d].
        const auto& candidate_expressions = candidate_od.ordering_expressions;
        if (ordered_expressions.size() < candidate_expressions.size() ||
            !first_expressions_match(candidate_expressions, ordered_expressions)) {
          continue;
        }

        const auto& transitive_od = OrderDependency(od.ordering_expressions, candidate_od.ordered_expressions);
        // Skip if OD is already known or OD would contain an expression both in LHS and RHS.
        if (order_dependencies.contains(transitive_od) ||
            std::any_of(transitive_od.ordering_expressions.cbegin(), transitive_od.ordering_expressions.cend(),
                        [&](const auto& expression) {
                          return find_expression_idx(*expression, candidate_od.ordered_expressions);
                        })) {
          continue;
        }

        // We cannot insert directly into order_dependencies since we still iterate over it and might invalidate the
        // iterator.
        transitive_ods.emplace_back(transitive_od);
      }
    }

    if (transitive_ods.empty()) {
      return;
    }

    order_dependencies.insert(transitive_ods.cbegin(), transitive_ods.cend());
  }
}

}  // namespace hyrise

namespace std {

size_t hash<hyrise::OrderDependency>::operator()(const hyrise::OrderDependency& od) const {
  return od.hash();
}

}  // namespace std
