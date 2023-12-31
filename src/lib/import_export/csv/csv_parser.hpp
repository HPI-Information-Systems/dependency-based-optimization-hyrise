#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "import_export/csv/csv_meta.hpp"

namespace hyrise {

class Table;
class Chunk;

/**
 * Creates a Table with values of the parsed csv file <filename> and
 * the corresponding meta file <filename>.meta
 *
 * The files are parsed according to RFC 4180 if not otherwise specified. [https://tools.ietf.org/html/rfc4180]
 * For non-RFC 4180, all linebreaks within quoted strings are further escaped with an escape character.
 * For the structure of the meta csv file see export_csv.hpp
 *
 * This parser reads the whole csv file and iterates over it to separate the data into chunks that are aligned with the
 * csv rows.
 * Each data chunk is parsed and converted into a Hyrise chunk. In the end all chunks are combined to the final table.
 */
class CsvParser {
 public:
  /*
   * @param filename      Path to the input file.
   * @param csv_meta      Custom csv meta information which will be used instead of the default "filename" + ".json" meta.
   * @returns             The table that was created from the csv file.
   */
  static std::shared_ptr<Table> parse(const std::string& filename, const ChunkOffset chunk_size = Chunk::DEFAULT_SIZE,
                                      const std::optional<CsvMeta>& csv_meta = std::nullopt);
  static std::shared_ptr<Table> create_table_from_meta_file(const std::string& filename,
                                                            const ChunkOffset chunk_size = Chunk::DEFAULT_SIZE);

 protected:
  /*
   * Use the meta information stored in _meta to create a new table with according column description.
   */
  static std::shared_ptr<Table> _create_table_from_meta(const ChunkOffset chunk_size, const CsvMeta& meta);

  /*
   * @param      csv_content String_view on the remaining content of the CSV.
   * @param      table       Empty table created by _process_meta_file.
   * @param[out] field_ends  Empty vector, to be filled with positions of the field ends for one chunk found in \p
   * csv_content.
   * @returns                False if \p csv_content is empty or chunk_size set to 0, True otherwise.
   */
  static bool _find_fields_in_chunk(std::string_view csv_content, const Table& table, std::vector<size_t>& field_ends,
                                    const CsvMeta& meta);

  /*
   * @param      csv_chunk  String_view on one chunk of the CSV.
   * @param      field_ends Positions of the field ends of the given \p csv_chunk.
   * @param      table      Empty table created by _process_meta_file.
   * @param[out] segments   The segments of the chunk, to be populated with data
   * @returns               The number of rows in the chunk
   */
  static size_t _parse_into_chunk(std::string_view csv_chunk, const std::vector<size_t>& field_ends, const Table& table,
                                  Segments& segments, const CsvMeta& meta, const std::string& escaped_linebreak,
                                  std::mutex& append_chunk_mutex);

  /*
   * @param field The field that needs to be modified to be RFC 4180 compliant.
   */
  static void _sanitize_field(std::string& field, const CsvMeta& meta, const std::string& escaped_linebreak);
};
}  // namespace hyrise
