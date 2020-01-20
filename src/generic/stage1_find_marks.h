// This file contains the common code every implementation uses in stage1
// It is intended to be included multiple times and compiled multiple times
// We assume the file in which it is included already includes
// "simdjson/stage1_find_marks.h" (this simplifies amalgation)

namespace stage1 {

class bit_indexer {
public:
  uint32_t *tail;

  bit_indexer(uint32_t *index_buf) : tail(index_buf) {}

  // flatten out values in 'bits' assuming that they are are to have values of idx
  // plus their position in the bitvector, and store these indexes at
  // base_ptr[base] incrementing base as we go
  // will potentially store extra values beyond end of valid bits, so base_ptr
  // needs to be large enough to handle this
  really_inline void write_indexes(uint32_t idx, uint64_t bits) {
    // In some instances, the next branch is expensive because it is mispredicted.
    // Unfortunately, in other cases,
    // it helps tremendously.
    if (bits == 0)
        return;
    uint32_t cnt = hamming(bits);

    // Do the first 8 all together
    for (int i=0; i<8; i++) {
      this->tail[i] = idx + trailing_zeroes(bits);
      bits = clear_lowest_bit(bits);
    }

    // Do the next 8 all together (we hope in most cases it won't happen at all
    // and the branch is easily predicted).
    if (unlikely(cnt > 8)) {
      for (int i=8; i<16; i++) {
        this->tail[i] = idx + trailing_zeroes(bits);
        bits = clear_lowest_bit(bits);
      }

      // Most files don't have 16+ structurals per block, so we take several basically guaranteed
      // branch mispredictions here. 16+ structurals per block means either punctuation ({} [] , :)
      // or the start of a value ("abc" true 123) every four characters.
      if (unlikely(cnt > 16)) {
        uint32_t i = 16;
        do {
          this->tail[i] = idx + trailing_zeroes(bits);
          bits = clear_lowest_bit(bits);
          i++;
        } while (i < cnt);
      }
    }

    this->tail += cnt;
  }
};

class json_structural_scanner {
public:
  // Whether the first character of the next iteration is escaped.
  uint64_t prev_escaped = 0ULL;
  // Whether we were still inside a string during the last iteration (all 1's = true, all 0's = false).
  uint64_t prev_in_string = 0ULL;
  // Overflow for the value series calculation we use to validate structure
  uint64_t prev_start_value = 0ULL;
  // Whether the last byte we primitive (i.e. can be followed by primitive or quote)
  uint64_t prev_primitive = 0ULL;
  // Whether the last thing we saw was a separator (i.e. , and spaces)
  uint64_t prev_separator = 0ULL;
  // Mask of structural characters from the last iteration.
  // Kept around for performance reasons, so we can call flatten_bits to soak up some unused
  // CPU capacity while the next iteration is busy with an expensive clmul in compute_quote_mask.
  uint64_t prev_structurals = 0;
  // Whether it has errors of any sort
  uint64_t has_error = 0;
  bit_indexer structural_indexes;

  json_structural_scanner(uint32_t *_structural_indexes) : structural_indexes{_structural_indexes} {}

  //
  // Finish the scan and return any errors.
  //
  // This may detect errors as well, such as unclosed string and certain UTF-8 errors.
  // if streaming is set to true, an unclosed string is allowed.
  //
  really_inline ErrorValues detect_errors_on_eof(bool streaming = false);

  //
  // Return a mask of all string characters plus end quotes.
  //
  // prev_escaped is overflow saying whether the next character is escaped.
  // prev_in_string is overflow saying whether we're still in a string.
  //
  // Backslash sequences outside of quotes will be detected in stage 2.
  //
  really_inline uint64_t find_strings(const simd::simd8x64<uint8_t> in);

  //
  // Determine which characters are *structural*:
  // - braces: [] and {}
  // - the start of primitives (123, true, false, null)
  // - the start of invalid non-whitespace (+, &, ture, UTF-8)
  //
  // Also detects value sequence errors:
  // - two values with no separator between ("hello" "world")
  // - separators with no values ([1,] [1,,]and [,2])
  //
  // This method will find all of the above whether it is in a string or not.
  //
  // To reduce dependency on the expensive "what is in a string" computation, this method treats the
  // contents of a string the same as content outside. Errors and structurals inside the string or on
  // the trailing quote will need to be removed later when the correct string information is known.
  //
  really_inline uint64_t find_potential_structurals(const simd::simd8x64<uint8_t> in);

  //
  // Find the important bits of JSON in a STEP_SIZE-byte chunk, and add them to structural_indexes.
  //
  template<size_t STEP_SIZE>
  really_inline void scan_step(const uint8_t *buf, const size_t idx, utf8_checker &utf8_checker);

  //
  // Parse the entire input in STEP_SIZE-byte chunks.
  //
  template<size_t STEP_SIZE>
  really_inline void scan(const uint8_t *buf, const size_t len, utf8_checker &utf8_checker);
};

really_inline uint64_t find_series(const uint64_t start, const uint64_t end, uint64_t &overflow) {
  uint64_t result;
  overflow = sub_overflow(end, start + overflow, &result);
  return result;
}

//
// Check if the current character immediately follows a matching character.
//
// For example, this checks for quotes with backslashes in front of them:
//
//     const uint64_t backslashed_quote = in.eq('"') & immediately_follows(in.eq('\'), prev_backslash);
//
really_inline uint64_t follows(const uint64_t match, uint64_t &overflow) {
  const uint64_t result = match << 1 | overflow;
  overflow = match >> 63;
  return result;
}


really_inline uint64_t follows_every_other(uint64_t match, uint64_t &overflow) {
  const uint64_t odd_bits = 0xAAAAAAAAAAAAAAAAULL;

  // Bring in the overflow
  uint64_t follows_match = match << 1 | overflow;

  // Get sequences starting on even bits by clearing out the odd series using +
  uint64_t odd_sequence_starts = match & odd_bits & ~follows_match;
  uint64_t sequences_starting_on_even_bits;
  overflow = add_overflow(odd_sequence_starts, follows_match, &sequences_starting_on_even_bits);

  // set even bits to even_bits, and flip odd bits using ^
  return (odd_bits ^ sequences_starting_on_even_bits) & follows_match;
}

//
// Check if the current character follows a matching character, with possible "filler" between.
// For example, this checks for empty curly braces, e.g. 
//
//     in.eq('}') & follows(in.eq('{'), in.eq(' '), prev_open_curly) // { <whitespace>* }
//
really_inline uint64_t follows_series(const uint64_t match, const uint64_t filler, uint64_t &overflow) {
  uint64_t result;
  overflow = add_overflow(match, match|filler, &result);
  return result;
}

really_inline ErrorValues json_structural_scanner::detect_errors_on_eof(bool streaming) {
  if ((prev_in_string) and (not streaming)) {
    return UNCLOSED_STRING;
  }
  if (prev_separator) {
    return TAPE_ERROR; // comma at the end is invalid
  }
  if (has_error) {
    return UNESCAPED_CHARS; // TODO also out of order JSON
  }
  // TODO validate that it either has one value, or starts with OPEN
  // (maybe that gets validated in the collection validator ...)
  return SUCCESS;
}

//
// Return a mask of all string characters plus end quotes.
//
// prev_escaped is overflow saying whether the next character is escaped.
// prev_in_string is overflow saying whether we're still in a string.
//
// Backslash sequences outside of quotes will be detected in stage 2.
//
really_inline uint64_t json_structural_scanner::find_strings(const simd::simd8x64<uint8_t> in, uint64_t quote) {
  const uint64_t backslash = in.eq('\\');
  const uint64_t escaped = follows_every_other(backslash, prev_escaped) << 1;
  const uint64_t real_quote = quote & ~escaped;
  // prefix_xor flips on bits inside the string (and flips off the end quote).
  const uint64_t in_string = prefix_xor(real_quote) ^ prev_in_string;
  /* right shift of a signed value expected to be well-defined and standard
  * compliant as of C++20,
  * John Regher from Utah U. says this is fine code */
  prev_in_string = static_cast<uint64_t>(static_cast<int64_t>(in_string) >> 63);
  // Use ^ to turn the beginning quote off, and the end quote on.
  return in_string ^ real_quote;
}

//
// Validate the sequence of expressions in the JSON:
//
// Open* Prim|Close Close* Separator ...
//
// To reduce dependency on the expensive "what is in a string" computation, this method treats the
// contents of a string the same as content outside. Errors and structurals inside the string or on
// the trailing quote will need to be removed later when the correct string information is known.
//
really_inline uint64_t json_structural_scanner::find_potential_structurals(const simd8x64<uint8_t> in, uint64_t quote, uint64_t &invalid_expression) {
  //
  // Classify the characters first ...
  //
  // 0x20 and under is whitespace. We're including plenty of invalid values here, but that's OK; we'll be validating those
  // separately. As long as this *doesn't* include operators or valid value characters, we're good.
  uint64_t space = (in <= 0x20).to_bitmask();
  simd8<uint8_t> to_curly = in | ('{' - '[');
  uint64_t open = (to_curly == '{').to_bitmask();
  uint64_t close = (to_curly == '}').to_bitmask();
  uint64_t curly = (open | close) & (in >= '{');
  uint64_t colon = (in == ':').to_bitmask();
  uint64_t separator = colon | (in == ',').to_bitmask();

  //
  // Validate the operators: Open Prim|Close Close* Separator ...
  //
  // Where Prim is any non-whitespace, non-operator.
  //
  // We subtract Separator - (Prim|Close) to show the following series:
  //
  //        <Open>             |            <Value><Close>             | <Separator>
  // (Separator | Open|Space)* | Prim|Close (Prim|Close | Open|Space)* | Separator
  //     ERR           0       |    1            0       ERR    1      |    0
  //
  uint64_t primitive = ~(separator|open|space|close);
  uint64_t start_value = find_series(primitive|close, separator, start_value_overflow);

  //
  // Validate: all of these examples are invalid *unless inside a string* (which we don't know yet):
  //
  // | Category  | Example | Detection                                  |
  // |-----------|---------|--------------------------------------------|
  // | Open      | `1 {`   | Open = 1                                   |
  // |           | `} {`   |                                            |
  // |-----------|---------|--------------------------------------------|
  // | Separator | `, ,`   | Separator = 1                              |
  // |           | `{ ,`   |                                            |
  // |-----------|---------|--------------------------------------------|
  // | Close     | `, }`   | Close = 1 MUST be empty:                   |
  // |           |         | - invalid=Separator Space* Close           |
  // |           |         | - valid=Open Space* Close                  |
  // |           |         | Detected later, in collection validation   |
  // |-----------|---------|--------------------------------------------|
  // | Primitive | `1 1`   | Primitive = 0 preceded by Space|Close      |
  // |           | `1}1`   |                                            |
  // |           | `}1`    |                                            |
  // |-----------|---------|--------------------------------------------|
  // | Quote     | `1"`    | Quote = 0 (any " after the first *must*    |
  // |           | `1"1`   | be in/end of a string).                    |
  // |           | `"""`   |                                            |
  // |           |         | Because of this, the tail quotes in `""`,  |
  // |           |         | `" "`, `\"` and `"\""` will end up valid   |
  // |           |         | (`\"` can occur in a document like         |
  // |           |         | `[1,2,"3,\""]` because separators can      |
  // |           |         | occur inside strings).                     |
  // |-----------|---------|--------------------------------------------|
  // | Space     |         | Whitespace is always cool.                 |
  // |-----------|---------|--------------------------------------------|
  //

  // Separator/Open = 1: `1 {`, `} {`, `, ,`, `{ ,`
  invalid_expression = (separator|open) & start_value;
  // Quote = 0: `1"` `1"1` `"""`
  invalid_expression |= quote & ~start_value;

  // Value: Error on `1 1`, `1}1`, `""1`, `}1`
  // All primitive chars except the first must be preceded by a non-quote primitive
  // (first " is OK)
  uint64_t tail_primitive = primitive & ~start_value;
  uint64_t tail_quote = quote & ~start_value;
  invalid_expression |= tail_primitive & follows(primitive & ~tail_quote, prev_primitive);

  // Close: Error on `, }`
  invalid_expression |= close & follows(separator, space, prev_separator);

  // NOTE: we have not attempted to validate individual characters yet: space and value
  // include a lot of invalid characters. Still, at *this* point we can at least get rid
  // of commas ...
  //
  // NOTE: at this point *only* quote&start_value can be the beginning of a string.
  // It's also possible it will be the end of a string, if the previous string
  // contained a separator. Finally, the only other thing that can possibly be an
  // end quote is the *last* primitive. Nothing in between. (We've already determined
  // this from valid/invalid expressions.) Backslashes only ever need checking on
  // the last quote, therefore, which will only ever happen if the string contains
  // both an end quote *and* a separator.
  // We could almost certainly forego clmul now!
  return start_value | open | close | colon;
}

//
// Find the important bits of JSON in a 128-byte chunk, and add them to structural_indexes.
//
// PERF NOTES:
// We pipe 2 inputs through these stages:
// 1. Load JSON into registers. This takes a long time and is highly parallelizable, so we load
//    2 inputs' worth at once so that by the time step 2 is looking for them input, it's available.
// 2. Scan the JSON for critical data: strings, primitives and operators. This is the critical path.
//    The output of step 1 depends entirely on this information. These functions don't quite use
//    up enough CPU: the second half of the functions is highly serial, only using 1 execution core
//    at a time. The second input's scans has some dependency on the first ones finishing it, but
//    they can make a lot of progress before they need that information.
// 3. Step 1 doesn't use enough capacity, so we run some extra stuff while we're waiting for that
//    to finish: utf-8 checks and generating the output from the last iteration.
// 
// The reason we run 2 inputs at a time, is steps 2 and 3 are *still* not enough to soak up all
// available capacity with just one input. Running 2 at a time seems to give the CPU a good enough
// workout.
//
template<>
really_inline void json_structural_scanner::scan_step<128>(const uint8_t *buf, const size_t idx, utf8_checker &utf8_checker) {
  //
  // Load up all 128 bytes into SIMD registers
  //
  simd::simd8x64<uint8_t> in_1(buf);
  simd::simd8x64<uint8_t> in_2(buf+64);

  //
  // Find the strings and potential structurals (operators / primitives).
  //
  // This will include false structurals that are *inside* strings--we'll filter strings out
  // before we return.
  //
  uint64_t quote_1 = in.eq('"');
  uint64_t string_1 = this->find_strings(in_1);
  uint64_t invalid_expression_1;
  uint64_t structurals_1 = this->find_potential_structurals(in_1, quote_1, invalid_expression_1);
  uint64_t quote_2 = in.eq('"');
  uint64_t invalid_expression_2;
  uint64_t string_2 = this->find_strings(in_2);
  uint64_t structurals_2 = this->find_potential_structurals(in_2, quote_2, invalid_expression_2);

  //
  // Do miscellaneous work while the processor is busy calculating strings and structurals.
  //
  // After that, weed out structurals that are inside strings and find invalid string characters.
  //
  uint64_t unescaped_1 = in_1.lteq(0x1F);
  utf8_checker.check_next_input(in_1);
  this->structural_indexes.write_indexes(idx-64, this->prev_structurals); // Output *last* iteration's structurals to ParsedJson
  this->prev_structurals = structurals_1 & ~string_1;
  this->has_error |= invalid_expression_1 & ~string_1;
  this->has_error |= unescaped_1 & string_1;

  uint64_t unescaped_2 = in_2.lteq(0x1F);
  utf8_checker.check_next_input(in_2);
  this->structural_indexes.write_indexes(idx, this->prev_structurals); // Output *last* iteration's structurals to ParsedJson
  this->prev_structurals = structurals_2 & ~string_2;
  this->has_error |= invalid_expression_2 & ~string_2;
  this->has_error |= unescaped_2 & string_2;
}

//
// Find the important bits of JSON in a 64-byte chunk, and add them to structural_indexes.
//
template<>
really_inline void json_structural_scanner::scan_step<64>(const uint8_t *buf, const size_t idx, utf8_checker &utf8_checker) {
  //
  // Load up bytes into SIMD registers
  //
  simd::simd8x64<uint8_t> in_1(buf);

  //
  // Find the strings and potential structurals (operators / primitives).
  //
  // This will include false structurals that are *inside* strings--we'll filter strings out
  // before we return.
  //
  uint64_t string_1 = this->find_strings(in_1);
  uint64_t structurals_1 = this->find_potential_structurals(in_1);

  //
  // Do miscellaneous work while the processor is busy calculating strings and structurals.
  //
  // After that, weed out structurals that are inside strings and find invalid string characters.
  //
  uint64_t unescaped_1 = in_1.lteq(0x1F);
  utf8_checker.check_next_input(in_1);
  this->structural_indexes.write_indexes(idx-64, this->prev_structurals); // Output *last* iteration's structurals to ParsedJson
  this->prev_structurals = structurals_1 & ~string_1;
  this->unescaped_chars_error |= unescaped_1 & string_1;
}

template<size_t STEP_SIZE>
really_inline void json_structural_scanner::scan(const uint8_t *buf, const size_t len, utf8_checker &utf8_checker) {
  size_t lenminusstep = len < STEP_SIZE ? 0 : len - STEP_SIZE;
  size_t idx = 0;

  for (; idx < lenminusstep; idx += STEP_SIZE) {
    this->scan_step<STEP_SIZE>(&buf[idx], idx, utf8_checker);
  }

  /* If we have a final chunk of less than STEP_SIZE bytes, pad it to STEP_SIZE with
  * spaces  before processing it (otherwise, we risk invalidating the UTF-8
  * checks). */
  if (likely(idx < len)) {
    uint8_t tmp_buf[STEP_SIZE];
    memset(tmp_buf, 0x20, STEP_SIZE);
    memcpy(tmp_buf, buf + idx, len - idx);
    this->scan_step<STEP_SIZE>(&tmp_buf[0], idx, utf8_checker);
    idx += STEP_SIZE;
  }

  /* finally, flatten out the remaining structurals from the last iteration */
  this->structural_indexes.write_indexes(idx-64, this->prev_structurals);
}

// Setting the streaming parameter to true allows the find_structural_bits to tolerate unclosed strings.
// The caller should still ensure that the input is valid UTF-8. If you are processing substrings,
// you may want to call on a function like trimmed_length_safe_utf8.
template<size_t STEP_SIZE>
int find_structural_bits(const uint8_t *buf, size_t len, simdjson::ParsedJson &pj, bool streaming) {
  if (unlikely(len > pj.byte_capacity)) {
    return simdjson::CAPACITY;
  }
  utf8_checker utf8_checker{};
  json_structural_scanner scanner{pj.structural_indexes.get()};
  scanner.scan<STEP_SIZE>(buf, len, utf8_checker);
  // we might tolerate an unclosed string if streaming is true
  simdjson::ErrorValues error = scanner.detect_errors_on_eof(streaming);
  if (unlikely(error != simdjson::SUCCESS)) {
    return error;
  }
  pj.n_structural_indexes = scanner.structural_indexes.tail - pj.structural_indexes.get();
  /* a valid JSON file cannot have zero structural indexes - we should have
   * found something */
  if (unlikely(pj.n_structural_indexes == 0u)) {
    return simdjson::EMPTY;
  }
  if (unlikely(pj.structural_indexes[pj.n_structural_indexes - 1] > len)) {
    return simdjson::UNEXPECTED_ERROR;
  }
  if (len != pj.structural_indexes[pj.n_structural_indexes - 1]) {
    /* the string might not be NULL terminated, but we add a virtual NULL
     * ending character. */
    pj.structural_indexes[pj.n_structural_indexes++] = len;
  }
  /* make it safe to dereference one beyond this array */
  pj.structural_indexes[pj.n_structural_indexes] = 0;
  return utf8_checker.errors();
}

} // namespace stage1