#include "join_node.hpp"

#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "expression/binary_predicate_expression.hpp"
#include "expression/expression_functional.hpp"
#include "expression/expression_utils.hpp"
#include "expression/lqp_column_expression.hpp"
#include "lqp_utils.hpp"
#include "operators/operator_join_predicate.hpp"
#include "types.hpp"
#include "utils/assert.hpp"

namespace hyrise {

using namespace expression_functional;  // NOLINT(build/namespaces)

JoinNode::JoinNode(const JoinMode init_join_mode) : AbstractLQPNode(LQPNodeType::Join), join_mode(init_join_mode) {
  Assert(join_mode == JoinMode::Cross, "Only Cross Joins can be constructed without predicate");
}

JoinNode::JoinNode(const JoinMode init_join_mode, const std::shared_ptr<AbstractExpression>& join_predicate)
    : JoinNode(init_join_mode, std::vector<std::shared_ptr<AbstractExpression>>{join_predicate}) {}

JoinNode::JoinNode(const JoinMode init_join_mode,
                   const std::vector<std::shared_ptr<AbstractExpression>>& init_join_predicates)
    : AbstractLQPNode(LQPNodeType::Join, init_join_predicates), join_mode(init_join_mode) {
  Assert(join_mode != JoinMode::Cross, "Cross Joins take no predicate");
  Assert(!join_predicates().empty(), "Non-Cross Joins require predicates");
}

std::string JoinNode::description(const DescriptionMode mode) const {
  const auto expression_mode = _expression_description_mode(mode);

  std::stringstream stream;
  stream << "[Join] Mode: " << join_mode;

  for (const auto& predicate : join_predicates()) {
    stream << " [" << predicate->description(expression_mode) << "]";
  }

  return stream.str();
}

std::vector<std::shared_ptr<AbstractExpression>> JoinNode::output_expressions() const {
  Assert(left_input() && right_input(), "Both inputs need to be set to determine a JoinNode's output expressions");

  /**
   * Update the JoinNode's output expressions every time they are requested. An overhead, but keeps the LQP code simple.
   * Previously we propagated _input_changed() calls through the LQP every time a node changed and that required a lot
   * of feeble code.
   */

  const auto& left_expressions = left_input()->output_expressions();
  const auto output_both_inputs = !is_semi_or_anti_join(join_mode);
  if (!output_both_inputs) {
    return left_expressions;
  }

  const auto& right_expressions = right_input()->output_expressions();
  auto output_expressions = std::vector<std::shared_ptr<AbstractExpression>>{};
  output_expressions.resize(left_expressions.size() + right_expressions.size());

  auto right_begin = std::copy(left_expressions.begin(), left_expressions.end(), output_expressions.begin());
  std::copy(right_expressions.begin(), right_expressions.end(), right_begin);

  return output_expressions;
}

UniqueColumnCombinations JoinNode::unique_column_combinations() const {
  // We cannot guarantee any UCCs for Cross-Joins.
  if (join_mode == JoinMode::Cross) {
    return UniqueColumnCombinations{};
  }

  // Semi- and Anti-Joins act as mere filters for input_left(). Thus, existing unique column combinations remain valid.
  if (is_semi_or_anti_join(join_mode)) {
    return _forward_left_unique_column_combinations();
  }

  const auto& left_unique_column_combinations = left_input()->unique_column_combinations();
  const auto& right_unique_column_combinations = right_input()->unique_column_combinations();

  return _output_unique_column_combinations(left_unique_column_combinations, right_unique_column_combinations);
}

UniqueColumnCombinations JoinNode::_output_unique_column_combinations(
    const UniqueColumnCombinations& left_unique_column_combinations,
    const UniqueColumnCombinations& right_unique_column_combinations) const {
  if (left_unique_column_combinations.empty() && right_unique_column_combinations.empty()) {
    // Early exit.
    return UniqueColumnCombinations{};
  }

  const auto predicates = join_predicates();
  if (predicates.empty() || predicates.size() > 1) {
    // No guarantees implemented yet for Cross Joins and multi-predicate joins.
    return UniqueColumnCombinations{};
  }

  DebugAssert(join_mode == JoinMode::Inner || join_mode == JoinMode::Left || join_mode == JoinMode::Right ||
                  join_mode == JoinMode::FullOuter,
              "Unhandled JoinMode");

  const auto join_predicate = std::dynamic_pointer_cast<BinaryPredicateExpression>(join_predicates().front());
  if (!join_predicate || join_predicate->predicate_condition != PredicateCondition::Equals) {
    // Also, no guarantees implemented yet for other join predicates than equals_() (Equi Join).
    return UniqueColumnCombinations{};
  }

  // Check uniqueness of join columns.
  const auto left_operand_is_unique =
      !left_unique_column_combinations.empty() &&
      contains_matching_unique_column_combination(left_unique_column_combinations, {join_predicate->left_operand()});
  const auto right_operand_is_unique =
      !right_unique_column_combinations.empty() &&
      contains_matching_unique_column_combination(right_unique_column_combinations, {join_predicate->right_operand()});

  if (left_operand_is_unique && right_operand_is_unique) {
    // Due to the one-to-one relationship, the UCCs of both sides remain valid.
    auto unique_column_combinations =
        UniqueColumnCombinations{left_unique_column_combinations.begin(), left_unique_column_combinations.end()};
    unique_column_combinations.insert(right_unique_column_combinations.cbegin(),
                                      right_unique_column_combinations.cend());
    return unique_column_combinations;
  }

  if (left_operand_is_unique) {
    // Uniqueness on the left prevents duplication of records on the right.
    return right_unique_column_combinations;
  }

  if (right_operand_is_unique) {
    // Uniqueness on the right prevents duplication of records on the left.
    return left_unique_column_combinations;
  }

  return UniqueColumnCombinations{};
}

OrderDependencies JoinNode::order_dependencies() const {
  if (is_semi_or_anti_join(join_mode)) {
    return _forward_left_order_dependencies();
  }

  // ODs are not affected from removing or duplicating tuples, so we simply have to foward left and right ODs without
  // duplicate ODs. We achieve deduplication by using sets for ODs.
  const auto& left_order_dependencies = left_input()->order_dependencies();
  const auto& right_order_dependencies = right_input()->order_dependencies();
  auto order_dependencies = OrderDependencies{left_order_dependencies.cbegin(), left_order_dependencies.cend()};
  order_dependencies.insert(right_order_dependencies.cbegin(), right_order_dependencies.cend());

  // For inner equi joins, new ODs can occur due to the transitive nature: Imagine a join on A.b = B.x where the input
  // nodes have the ODs A.a |-> A.b, and B.x |-> B.y. In this case, the two additional ODs A.a |-> B.x and A.a |-> B.y
  // occur. For now, we limit the transitive closure of ODs to joins with a single equals predicate. Otherwise, we would
  // have to add all permutations of the predicates as ODs.
  const auto& predicates = join_predicates();
  if (join_mode != JoinMode::Inner || predicates.size() != 1) {
    return order_dependencies;
  }

  const auto& binary_predicate = std::dynamic_pointer_cast<BinaryPredicateExpression>(predicates.front());
  if (!binary_predicate || binary_predicate->predicate_condition != PredicateCondition::Equals) {
    return order_dependencies;
  }

  const auto& join_key_1 = binary_predicate->left_operand();
  const auto& join_key_2 = binary_predicate->right_operand();

  // Return if this is a self join and we already have ODs. This makes things tricky when we build the transitive
  // closure: If the join columns are different, we do not know to which of the output columns new ODs apply.
  const auto get_original_node = [](const auto& expression) {
    // Skip complex join keys.
    if (expression->type != ExpressionType::LQPColumn) {
      return std::shared_ptr<const AbstractLQPNode>{};
    }

    return static_cast<LQPColumnExpression&>(*expression).original_node.lock();
  };

  const auto& original_node_1 = get_original_node(join_key_1);
  if (!original_node_1 || *original_node_1 == *get_original_node(join_key_2)) {
    return order_dependencies;
  }

  // Join keys are equal, so the OD is bidirectional.
  order_dependencies.emplace(expression_vector(join_key_1), expression_vector(join_key_2));
  order_dependencies.emplace(expression_vector(join_key_2), expression_vector(join_key_1));

  build_transitive_od_closure(order_dependencies);
  return order_dependencies;
}

InclusionDependencies JoinNode::inclusion_dependencies() const {
  switch (join_mode) {
    case JoinMode::Cross:
    case JoinMode::FullOuter: {
      // These joins preserve all tuples from the inputs. All values survive and we can forward all INDs of the left and
      // right input.
      const auto& left_inclusion_dependencies = left_input()->inclusion_dependencies();
      const auto& right_inclusion_dependencies = right_input()->inclusion_dependencies();
      auto inclusion_dependencies =
          InclusionDependencies{left_inclusion_dependencies.cbegin(), left_inclusion_dependencies.cend()};
      inclusion_dependencies.insert(right_inclusion_dependencies.cbegin(), right_inclusion_dependencies.cend());
      return inclusion_dependencies;
    }

    // Anti-joins filter the left input. No INDs remain valid.
    case JoinMode::AntiNullAsFalse:
    case JoinMode::AntiNullAsTrue:
      return InclusionDependencies{};

    // All other joins can only forward INDs of the inputs if all input tuples are forwarded (i.e., (i) they are outer
    // joins and/or (ii) there is an IND between the join keys of equals_() predicates).
    case JoinMode::Left:
    case JoinMode::Right:
    case JoinMode::Semi:
    case JoinMode::Inner: {
      const auto& left_inclusion_dependencies = left_input()->inclusion_dependencies();
      const auto& right_inclusion_dependencies = right_input()->inclusion_dependencies();
      return _output_inclusion_dependencies(left_inclusion_dependencies, right_inclusion_dependencies);
    }
  }
  /**
   * Future work: The join keys of equals_() predicates form a new IND. However, we currently only care about INDs that
   * result from foreign key constraints.
   */
}

InclusionDependencies JoinNode::_output_inclusion_dependencies(
    // TODO: test!
    const InclusionDependencies& left_inclusion_dependencies,
    const InclusionDependencies& right_inclusion_dependencies) const {
  // Check if there are any INDs that might be forwarded.
  if (left_inclusion_dependencies.empty() && (join_mode == JoinMode::Semi || right_inclusion_dependencies.empty())) {
    return InclusionDependencies{};
  }

  // Left / right outer joins foward all tuples of the repsective input. Its INDs remain valid.
  auto inclusion_dependencies = InclusionDependencies{};
  if (join_mode == JoinMode::Left) {
    inclusion_dependencies.insert(left_inclusion_dependencies.cbegin(), left_inclusion_dependencies.cend());
  } else if (join_mode == JoinMode::Right) {
    inclusion_dependencies.insert(right_inclusion_dependencies.cbegin(), right_inclusion_dependencies.cend());
  }

  // Check that all join predicates are equals_() predicates and map the join keys to the input nodes.
  const auto join_predicates = this->join_predicates();
  const auto predicate_count = join_predicates.size();
  auto left_input_join_predicates = ExpressionUnorderedSet{predicate_count};
  auto right_input_join_predicates = ExpressionUnorderedSet{predicate_count};
  const auto& left_expressions = left_input()->output_expressions();

  for (const auto& expression : join_predicates) {
    const auto& predicate = std::dynamic_pointer_cast<BinaryPredicateExpression>(expression);
    if (!predicate || predicate->predicate_condition != PredicateCondition::Equals) {
      return InclusionDependencies{};
    }

    if (find_expression_idx(*predicate->left_operand(), left_expressions)) {
      DebugAssert(find_expression_idx(*predicate->right_operand(), right_input()->output_expressions()),
                  "Expected to resolve right operand.");
      left_input_join_predicates.emplace(predicate->left_operand());
      right_input_join_predicates.emplace(predicate->right_operand());
    } else {
      DebugAssert(find_expression_idx(*predicate->left_operand(), right_input()->output_expressions()),
                  "Expected to resolve left operand.");
      DebugAssert(find_expression_idx(*predicate->right_operand(), left_expressions),
                  "Expected to resolve right operand.");
      left_input_join_predicates.emplace(predicate->right_operand());
      right_input_join_predicates.emplace(predicate->left_operand());
    }
  }

  Assert(left_input_join_predicates.size() == predicate_count && right_input_join_predicates.size() == predicate_count,
         "Could not resolve all join predicates.");

  // Add INDs from left input if left join keys are contained in right join keys.
  if (right_input()->has_matching_ind(left_input_join_predicates, right_input_join_predicates, *left_input())) {
    inclusion_dependencies.insert(left_inclusion_dependencies.cbegin(), left_inclusion_dependencies.cend());
  }

  if (join_mode == JoinMode::Semi) {
    return inclusion_dependencies;
  }

  // Add INDs from right input if right join keys are contained in left join keys.
  if (left_input()->has_matching_ind(right_input_join_predicates, left_input_join_predicates, *right_input())) {
    inclusion_dependencies.insert(right_inclusion_dependencies.cbegin(), right_inclusion_dependencies.cend());
  }

  return inclusion_dependencies;
}

FunctionalDependencies JoinNode::non_trivial_functional_dependencies() const {
  /**
   * In the case of Semi- & Anti-Joins, this node acts as a filter for the left input node. The number of output
   * expressions does not change and therefore we should forward non-trivial FDs as follows:
   */
  if (is_semi_or_anti_join(join_mode)) {
    return left_input()->non_trivial_functional_dependencies();
  }

  /**
   * When joining tables, we usually lose some or even all unique column combinations from both input tables. This leads
   * to fewer trivial FDs that we can generate from UCCs in upper nodes. To preserve all possible FDs, we manually
   * forward all FDs from input nodes with discarded UCCs.
   */
  auto fds_left = FunctionalDependencies{};
  auto fds_right = FunctionalDependencies{};

  const auto& left_unique_column_combinations = left_input()->unique_column_combinations();
  const auto& right_unique_column_combinations = right_input()->unique_column_combinations();
  const auto& output_unique_column_combinations =
      _output_unique_column_combinations(left_unique_column_combinations, right_unique_column_combinations);

  if (output_unique_column_combinations.empty() && !left_unique_column_combinations.empty() &&
      !right_unique_column_combinations.empty()) {
    // Left and right UCCs are discarded, so we have to manually forward all FDs from the input nodes.
    fds_left = left_input()->functional_dependencies();
    fds_right = right_input()->functional_dependencies();
  } else if ((output_unique_column_combinations.empty() ||
              output_unique_column_combinations == right_unique_column_combinations) &&
             !left_unique_column_combinations.empty()) {
    // Left UCCs are discarded, so we have to manually forward all FDs of the left input node.
    fds_left = left_input()->functional_dependencies();
    fds_right = right_input()->non_trivial_functional_dependencies();
  } else if ((output_unique_column_combinations.empty() ||
              output_unique_column_combinations == left_unique_column_combinations) &&
             !right_unique_column_combinations.empty()) {
    // Right UCCs are discarded, so we have to manually forward all FDs of the right input node.
    fds_left = left_input()->non_trivial_functional_dependencies();
    fds_right = right_input()->functional_dependencies();
  } else {
    // No UCCs are discarded. We only have to forward non-trivial FDs.
    DebugAssert(output_unique_column_combinations.size() ==
                    (left_unique_column_combinations.size() + right_unique_column_combinations.size()),
                "Unexpected number of unique constraints.");
    fds_left = left_input()->non_trivial_functional_dependencies();
    fds_right = right_input()->non_trivial_functional_dependencies();
  }

  // Prevent FDs with duplicate determinant expressions in the output vector.
  auto fds_out = union_fds(fds_left, fds_right);

  // Outer joins lead to nullable columns, which may invalidate some FDs.
  if (!fds_out.empty() &&
      (join_mode == JoinMode::FullOuter || join_mode == JoinMode::Left || join_mode == JoinMode::Right)) {
    remove_invalid_fds(shared_from_this(), fds_out);
  }

  /**
   * Future Work: In some cases, it is possible to create FDs from the join columns.
   *              For example: a) {join_column_a} => {join_column_b}
   *                           b) {join_column_b} => {join_column_a}
   */

  return fds_out;
}

bool JoinNode::is_column_nullable(const ColumnID column_id) const {
  Assert(left_input() && right_input(), "Need both inputs to determine nullability");

  const auto left_input_column_count = left_input()->output_expressions().size();
  const auto column_is_from_left_input = column_id < left_input_column_count;

  if (join_mode == JoinMode::Left && !column_is_from_left_input) {
    return true;
  }

  if (join_mode == JoinMode::Right && column_is_from_left_input) {
    return true;
  }

  if (join_mode == JoinMode::FullOuter) {
    return true;
  }

  if (column_is_from_left_input) {
    return left_input()->is_column_nullable(column_id);
  }

  const auto right_column_id =
      static_cast<ColumnID>(column_id - static_cast<ColumnID::base_type>(left_input_column_count));
  return right_input()->is_column_nullable(right_column_id);
}

const std::vector<std::shared_ptr<AbstractExpression>>& JoinNode::join_predicates() const {
  return node_expressions;
}

void JoinNode::mark_as_semi_reduction(const std::shared_ptr<JoinNode>& reduced_join_node) {
  Assert(!_is_semi_reduction, "The semi reduction status should be set once only.");
  Assert(reduced_join_node, "Reduced JoinNode must be provided.");
  Assert(join_mode == JoinMode::Semi, "Semi join reductions require JoinMode::Semi.");
  DebugAssert(join_predicates().size() == 1,
              "Currently, semi join reductions are expected to have a single join predicate.");
  DebugAssert(std::any_of(reduced_join_node->join_predicates().cbegin(), reduced_join_node->join_predicates().cend(),
                          [&](const auto predicate) { return *predicate == *join_predicates()[0]; }),
              "Both semi join reduction node and the reduced join should have a common join predicate.");
  _is_semi_reduction = true;
  _reduced_join_node = std::weak_ptr<JoinNode>(reduced_join_node);
}

void JoinNode::mark_input_side_as_prunable(LQPInputSide input_side) {
  _prunable_input_side = input_side;
}

bool JoinNode::is_semi_reduction() const {
  DebugAssert(!_is_semi_reduction || join_mode == JoinMode::Semi, "Non-semi join is marked as a semi reduction.");
  return _is_semi_reduction;
}

std::optional<LQPInputSide> JoinNode::prunable_input_side() const {
  if (is_semi_or_anti_join(join_mode)) {
    return LQPInputSide::Right;
  }
  return _prunable_input_side;
}

std::shared_ptr<JoinNode> JoinNode::get_or_find_reduced_join_node() const {
  Assert(_is_semi_reduction, "Expected semi join reduction node.");

  if (_reduced_join_node.expired()) {
    // In deep copies of the LQP, the weak pointer to the reduced join is unset (lazy discovery). In such cases,
    // find the reduced join by traversing the LQP upwards.
    const auto& reduction_predicate = *join_predicates()[0];
    visit_lqp_upwards(std::const_pointer_cast<AbstractLQPNode>(shared_from_this()), [&](const auto& current_node) {
      if (current_node->type != LQPNodeType::Join || current_node.get() == this) {
        return LQPUpwardVisitation::VisitOutputs;
      }
      const auto join_node = std::static_pointer_cast<JoinNode>(current_node);
      if (std::none_of(join_node->join_predicates().begin(), join_node->join_predicates().end(),
                       [&](const auto& predicate) { return *predicate == reduction_predicate; })) {
        return LQPUpwardVisitation::VisitOutputs;
      }

      _reduced_join_node = std::weak_ptr<JoinNode>(join_node);
      return LQPUpwardVisitation::DoNotVisitOutputs;
    });

    Assert(!_reduced_join_node.expired(), "Could not find JoinNode that gets reduced by this semi join reduction.");
  }

  return _reduced_join_node.lock();
}

size_t JoinNode::_on_shallow_hash() const {
  size_t hash = boost::hash_value(join_mode);
  boost::hash_combine(hash, _is_semi_reduction);
  return hash;
}

std::shared_ptr<AbstractLQPNode> JoinNode::_on_shallow_copy(LQPNodeMapping& node_mapping) const {
  if (join_predicates().empty()) {
    Assert(join_mode == JoinMode::Cross, "Expected cross join.");
    return JoinNode::make(join_mode);
  }
  const auto copied_join_node =
      JoinNode::make(join_mode, expressions_copy_and_adapt_to_different_lqp(join_predicates(), node_mapping));
  copied_join_node->_is_semi_reduction = _is_semi_reduction;
  return copied_join_node;
}

bool JoinNode::_on_shallow_equals(const AbstractLQPNode& rhs, const LQPNodeMapping& node_mapping) const {
  const auto& join_node = static_cast<const JoinNode&>(rhs);
  if (join_mode != join_node.join_mode || _is_semi_reduction != join_node._is_semi_reduction) {
    return false;
  }
  return expressions_equal_to_expressions_in_different_lqp(join_predicates(), join_node.join_predicates(),
                                                           node_mapping);
}

}  // namespace hyrise
