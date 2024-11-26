#include "export.hpp"

#include <boost/algorithm/string.hpp>
#include "magic_enum.hpp"

#include "hyrise.hpp"
#include "import_export/binary/binary_writer.hpp"
#include "import_export/csv/csv_writer.hpp"
#include "storage/chunk_encoder.hpp"
#include "storage/segment_iterate.hpp"
#include "storage/table.hpp"
#include "storage/value_segment.hpp"
#include "utils/assert.hpp"

namespace hyrise {

Export::Export(const std::shared_ptr<const AbstractOperator>& input_operator, const std::string& filename,
               const FileType& file_type)
    : AbstractReadOnlyOperator(OperatorType::Export, input_operator), _filename(filename), _file_type(file_type) {
  if (_file_type == FileType::Auto) {
    _file_type = file_type_from_filename(filename);
  }
}

const std::string& Export::name() const {
  static const auto name = std::string{"Export"};
  return name;
}

std::string Export::description(DescriptionMode description_mode) const {
  const auto separator = (description_mode == DescriptionMode::SingleLine ? ' ' : '\n');

  auto file_type = std::string{magic_enum::enum_name(_file_type)};
  boost::algorithm::to_lower(file_type);
  return AbstractOperator::description(description_mode) + separator + "to '" + _filename + "'" + separator + "(" +
         file_type + ")";
}

std::shared_ptr<const Table> Export::_on_execute() {
  if (_filename.empty() || std::all_of(_filename.begin(), _filename.end(), isspace)) {
    Fail("Export: File name must not be empty.");
  }

  auto table = left_input_table();
  if (_file_type == FileType::Binary && table->type() == TableType::References) {
    const auto new_table = std::make_shared<Table>(table->column_definitions(), TableType::Data, Chunk::DEFAULT_SIZE);
    const auto chunk_count = table->chunk_count();
    const auto column_count = table->column_count();
    for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
      const auto& chunk = table->get_chunk(chunk_id);
      Assert(chunk, "Physically deleted chunk should not reach this point.");
      auto segments = Segments{};
      for (auto column_id = ColumnID{0}; column_id < column_count; ++column_id) {
        const auto source_segment = chunk->get_segment(column_id);

        resolve_data_type(source_segment->data_type(), [&](auto type) {
          using SegmentDataType = typename decltype(type)::type;

          auto values = pmr_vector<SegmentDataType>(source_segment->size());
          auto null_values = pmr_vector<bool>{};
          if (table->column_is_nullable(column_id)) {
            null_values.resize(source_segment->size());
          }
          auto current_position = size_t{0};

          segment_iterate<SegmentDataType>(*source_segment, [&](const auto& position) {
            if (position.is_null()) {
              null_values[current_position] = true;
            } else {
              values[current_position] = position.value();
            }
            ++current_position;
          });

          if (table->column_is_nullable(column_id)) {
            segments.emplace_back(
                std::make_shared<ValueSegment<SegmentDataType>>(std::move(values), std::move(null_values)));
          } else {
            segments.emplace_back(std::make_shared<ValueSegment<SegmentDataType>>(std::move(values)));
          }
        });
      }
      new_table->append_chunk(segments);
      new_table->last_chunk()->finalize();
    }

    ChunkEncoder::encode_all_chunks(new_table);
    table = new_table;
  }

  switch (_file_type) {
    case FileType::Csv:
      CsvWriter::write(*table, _filename);
      break;
    case FileType::Binary:
      BinaryWriter::write(*table, _filename);
      break;
    case FileType::Auto:
    case FileType::Tbl:
      Fail("Export: Exporting file type is not supported.");
  }

  // must match ExportNode::output_expressions
  return nullptr;
}

std::shared_ptr<AbstractOperator> Export::_on_deep_copy(
    const std::shared_ptr<AbstractOperator>& copied_left_input,
    const std::shared_ptr<AbstractOperator>& /*copied_right_input*/,
    std::unordered_map<const AbstractOperator*, std::shared_ptr<AbstractOperator>>& /*copied_ops*/) const {
  return std::make_shared<Export>(copied_left_input, _filename, _file_type);
}

void Export::_on_set_parameters(const std::unordered_map<ParameterID, AllTypeVariant>& parameters) {}

}  // namespace hyrise
