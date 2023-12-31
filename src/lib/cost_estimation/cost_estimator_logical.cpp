#include "cost_estimator_logical.hpp"

#include "expression/abstract_expression.hpp"
#include "expression/expression_utils.hpp"
#include "expression/lqp_subquery_expression.hpp"
#include "logical_query_plan/abstract_lqp_node.hpp"
#include "logical_query_plan/join_node.hpp"
#include "logical_query_plan/predicate_node.hpp"
#include "logical_query_plan/union_node.hpp"
#include "statistics/cardinality_estimator.hpp"

namespace {

using namespace hyrise;  // NOLINT(build/namespaces)

float expression_cost_multiplier(const std::shared_ptr<AbstractExpression>& expression) {
  auto multiplier = 0.0f;

  // Number of different columns accessed to factor in expression complexity. Also add a factor for correlated
  // subqueries since we have to evaluate the subquery for each tuple again.
  visit_expression(expression, [&](const auto& sub_expression) {
    if ((sub_expression->type == ExpressionType::LQPColumn) ||
        (sub_expression->type == ExpressionType::LQPSubquery &&
         static_cast<LQPSubqueryExpression&>(*sub_expression).is_correlated())) {
      multiplier += 1.0f;
    }

    return ExpressionVisitation::VisitArguments;
  });

  return std::max(1.0f, multiplier);
}

}  // namespace

namespace hyrise {

std::shared_ptr<AbstractCostEstimator> CostEstimatorLogical::new_instance() const {
  return std::make_shared<CostEstimatorLogical>(cardinality_estimator->new_instance());
}

Cost CostEstimatorLogical::estimate_node_cost(const std::shared_ptr<AbstractLQPNode>& node) const {
  const auto output_row_count = cardinality_estimator->estimate_cardinality(node);
  const auto left_input_row_count =
      node->left_input() ? cardinality_estimator->estimate_cardinality(node->left_input()) : 0.0f;
  const auto right_input_row_count =
      node->right_input() ? cardinality_estimator->estimate_cardinality(node->right_input()) : 0.0f;

  switch (node->type) {
    case LQPNodeType::Join:
      // Covers predicated and unpredicated joins. For cross joins, output_row_count will be
      // left_input_row_count * right_input_row_count.
      return left_input_row_count + right_input_row_count + output_row_count;

    case LQPNodeType::Sort:
      // n * log(n) for sorting, plus n for output writing.
      return left_input_row_count * std::log(left_input_row_count) + output_row_count;

    case LQPNodeType::Union: {
      const auto& union_mode = static_cast<const UnionNode&>(*node).set_operation_mode;
      Assert(union_mode == SetOperationMode::Positions,
             "Invalid Union mode :" + std::string{magic_enum::enum_name(union_mode)});

      switch (union_mode) {
        case SetOperationMode::Positions:
          // To merge the PosLists, we have to sort them. Thus, n * log(n) for each input plus output writing.
          return left_input_row_count * std::log(left_input_row_count) +
                 right_input_row_count * std::log(right_input_row_count) + output_row_count;
        case SetOperationMode::All:
          // UnionAll simply appends its two inputs and does not touch the actual data.
          return 0.0f;
        case SetOperationMode::Unique:
          Fail("ToDo, see discussion https://github.com/hyrise/hyrise/pull/2156#discussion_r452803825");
      }

      Fail("This cannot happen, but gcc thinks this is a fall-through and complains.");
    }

    case LQPNodeType::StoredTable:
      // Simply forwards segments, does not touch the data.
      return 0.0f;

    case LQPNodeType::Predicate: {
      const auto& predicate = static_cast<const PredicateNode&>(*node).predicate();
      // n * number of scanned columns + output writing.
      return left_input_row_count * expression_cost_multiplier(predicate) + output_row_count;
    }

    default:
      return left_input_row_count + output_row_count;
  }
}

}  // namespace hyrise
