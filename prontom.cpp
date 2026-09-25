#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::ordered_json;

static constexpr const char* PRONTOM_VERSION = "2.0.0";

struct Rational {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    Rational() = default;
    Rational(std::int64_t n, std::int64_t d) {
        if (d == 0) {
            throw std::invalid_argument("zero denominator");
        }
        if (d < 0) {
            n = -n;
            d = -d;
        }
        const auto divisor = std::gcd(n < 0 ? -n : n, d);
        numerator = n / divisor;
        denominator = d / divisor;
    }
};

static bool operator<(const Rational& left, const Rational& right) {
    return static_cast<__int128>(left.numerator) * right.denominator
        < static_cast<__int128>(right.numerator) * left.denominator;
}

static bool operator<=(const Rational& left, const Rational& right) {
    return !(right < left);
}

static bool operator==(const Rational& left, const Rational& right) {
    return left.numerator == right.numerator
        && left.denominator == right.denominator;
}

static Rational operator-(const Rational& left, const Rational& right) {
    const auto numerator = static_cast<__int128>(left.numerator) * right.denominator
        - static_cast<__int128>(right.numerator) * left.denominator;
    const auto denominator = static_cast<__int128>(left.denominator) * right.denominator;
    if (numerator < INT64_MIN || numerator > INT64_MAX
        || denominator > INT64_MAX) {
        throw std::overflow_error("beat value is too large");
    }
    return Rational(static_cast<std::int64_t>(numerator),
                    static_cast<std::int64_t>(denominator));
}

static Rational operator*(const Rational& left, std::int64_t right) {
    const auto numerator = static_cast<__int128>(left.numerator) * right;
    if (numerator < INT64_MIN || numerator > INT64_MAX) {
        throw std::overflow_error("beat value is too large");
    }
    return Rational(static_cast<std::int64_t>(numerator), left.denominator);
}

static Rational operator/(const Rational& left, std::int64_t right) {
    if (right == 0) {
        throw std::invalid_argument("division by zero");
    }
    const auto denominator = static_cast<__int128>(left.denominator) * right;
    if (denominator < INT64_MIN || denominator > INT64_MAX) {
        throw std::overflow_error("beat value is too large");
    }
    return Rational(left.numerator, static_cast<std::int64_t>(denominator));
}

static std::string rational_to_string(const Rational& value) {
    if (value.denominator == 1) {
        return std::to_string(value.numerator);
    }
    return std::to_string(value.numerator) + "/" +
        std::to_string(value.denominator);
}

static std::optional<std::int64_t> python_int(const json& value) {
    try {
        if (value.is_number_integer()) {
            return value.get<std::int64_t>();
        }
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number <= INT64_MAX) {
                return static_cast<std::int64_t>(number);
            }
            return std::nullopt;
        }
        if (value.is_number_float()) {
            const auto number = value.get<double>();
            if (!std::isfinite(number)
                || number < static_cast<double>(INT64_MIN)
                || number > static_cast<double>(INT64_MAX)) {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(number);
        }
        if (value.is_string()) {
            const auto text = value.get<std::string>();
            std::size_t position = 0;
            const auto result = std::stoll(text, &position, 10);
            if (position != text.size()) {
                return std::nullopt;
            }
            return result;
        }
        if (value.is_boolean()) {
            return value.get<bool>() ? 1 : 0;
        }
    } catch (...) {
    }
    return std::nullopt;
}

static bool is_valid_column(const json& value, std::int64_t source_columns) {
    return value.is_number_integer()
        && !value.is_boolean()
        && value.get<std::int64_t>() >= 0
        && value.get<std::int64_t>() < source_columns;
}

static std::optional<Rational> beat_value(const json& note) {
    if (!note.is_object() || !note.contains("beat")
        || !note["beat"].is_array() || note["beat"].size() < 3) {
        return std::nullopt;
    }
    const auto integer = python_int(note["beat"][0]);
    const auto numerator = python_int(note["beat"][1]);
    const auto denominator = python_int(note["beat"][2]);
    if (!integer || !numerator || !denominator || *denominator == 0) {
        return std::nullopt;
    }
    try {
        const auto combined = static_cast<__int128>(*integer) * *denominator
            + *numerator;
        if (combined < INT64_MIN || combined > INT64_MAX) {
            return std::nullopt;
        }
        return Rational(static_cast<std::int64_t>(combined), *denominator);
    } catch (...) {
        return std::nullopt;
    }
}

static std::int64_t read_source_columns(
    const json& data,
    const std::vector<json>& ordinary_notes
) {
    try {
        const auto& value = data.at("meta").at("mode_ext").at("column");
        const auto parsed = python_int(value);
        if (parsed && *parsed > 0) {
            return *parsed;
        }
    } catch (...) {
    }

    std::optional<std::int64_t> maximum;
    for (const auto& note : ordinary_notes) {
        if (!note.is_object() || !note.contains("column")) {
            continue;
        }
        const auto& value = note["column"];
        if (value.is_number_integer() && !value.is_boolean()) {
            const auto column = value.get<std::int64_t>();
            if (column >= 0 && (!maximum || column > *maximum)) {
                maximum = column;
            }
        }
    }
    return maximum ? *maximum + 1 : 4;
}

static std::vector<std::vector<std::int64_t>> build_integer_matrix(
    std::int64_t source_columns,
    std::int64_t target_columns,
    std::int64_t& period
) {
    const auto divisor = std::gcd(source_columns, target_columns);
    period = target_columns / divisor;
    std::vector<std::vector<std::int64_t>> matrix(
        static_cast<std::size_t>(source_columns),
        std::vector<std::int64_t>(static_cast<std::size_t>(target_columns))
    );

    for (std::int64_t source = 0; source < source_columns; ++source) {
        std::int64_t row_sum = 0;
        for (std::int64_t target = 0; target < target_columns; ++target) {
            const auto overlap = std::min(
                target_columns * (source + 1),
                source_columns * (target + 1)
            ) - std::max(
                target_columns * source,
                source_columns * target
            );
            matrix[source][target] = overlap > 0 ? overlap / divisor : 0;
            row_sum += matrix[source][target];
        }
        if (row_sum != period) {
            throw std::runtime_error("Internal error: invalid matrix row sum");
        }
    }

    const auto expected_column_sum = source_columns / divisor;
    for (std::int64_t target = 0; target < target_columns; ++target) {
        std::int64_t column_sum = 0;
        for (std::int64_t source = 0; source < source_columns; ++source) {
            column_sum += matrix[source][target];
        }
        if (column_sum != expected_column_sum) {
            throw std::runtime_error("Internal error: invalid matrix column sum");
        }
    }
    return matrix;
}

static std::vector<std::int64_t> build_balanced_cycle(
    const std::vector<std::int64_t>& row
) {
    const auto period = std::accumulate(row.begin(), row.end(), std::int64_t{0});
    auto remaining = row;
    std::vector<std::int64_t> used(row.size(), 0);
    std::vector<std::int64_t> cycle;
    cycle.reserve(static_cast<std::size_t>(period));

    for (std::int64_t step = 0; step < period; ++step) {
        std::optional<std::int64_t> best_target;
        __int128 best_score = 0;
        for (std::int64_t target = 0;
             target < static_cast<std::int64_t>(row.size());
             ++target) {
            if (remaining[target] <= 0) {
                continue;
            }
            const auto score = static_cast<__int128>(step + 1) * row[target]
                - static_cast<__int128>(used[target]) * period;
            if (!best_target || score > best_score) {
                best_score = score;
                best_target = target;
            }
        }
        if (!best_target) {
            throw std::runtime_error("Internal error: empty balanced cycle");
        }
        cycle.push_back(*best_target);
        --remaining[*best_target];
        ++used[*best_target];
    }

    if (std::any_of(remaining.begin(), remaining.end(),
                    [](std::int64_t value) { return value != 0; })) {
        throw std::runtime_error("Internal error: incomplete balanced cycle");
    }
    return cycle;
}

static std::int64_t rotated_index(std::int64_t index, std::int64_t offset,
                                   std::int64_t period) {
    return (index + offset) % period;
}

static std::vector<std::int64_t> choose_phase_offsets(
    const std::vector<std::vector<std::int64_t>>& cycles,
    const std::vector<std::vector<std::int64_t>>& matrix,
    std::int64_t target_columns
) {
    const auto period = cycles.empty()
        ? 0
        : static_cast<std::int64_t>(cycles[0].size());
    std::vector<std::int64_t> chosen;

    for (std::int64_t source = 0;
         source < static_cast<std::int64_t>(cycles.size());
         ++source) {
        if (period == 0) {
            chosen.push_back(0);
            continue;
        }
        std::int64_t best_offset = 0;
        std::optional<std::int64_t> best_penalty;

        for (std::int64_t offset = 0; offset < period; ++offset) {
            std::int64_t penalty = 0;
            for (std::int64_t previous = 0;
                 previous < static_cast<std::int64_t>(chosen.size());
                 ++previous) {
                bool shares_lane = false;
                for (std::int64_t target = 0; target < target_columns; ++target) {
                    if (matrix[source][target] > 0
                        && matrix[previous][target] > 0) {
                        shares_lane = true;
                        break;
                    }
                }
                if (!shares_lane) {
                    continue;
                }
                for (std::int64_t phase = 0; phase < period; ++phase) {
                    const auto left = cycles[source][
                        static_cast<std::size_t>(rotated_index(phase, offset, period))
                    ];
                    const auto right = cycles[previous][
                        static_cast<std::size_t>(rotated_index(
                            phase, chosen[previous], period
                        ))
                    ];
                    if (left == right) {
                        ++penalty;
                    }
                }
            }
            if (!best_penalty || penalty < *best_penalty) {
                best_penalty = penalty;
                best_offset = offset;
            }
        }
        chosen.push_back(best_offset);
    }
    return chosen;
}

struct Gap {
    bool infinite = false;
    Rational value;
};

struct HoldInfo {
    std::size_t note_index = 0;
    std::int64_t source = 0;
    std::int64_t target = 0;
    Rational start;
    Rational end;
};

struct HoldProcessStats {
    std::int64_t trimmed_holds = 0;
    std::int64_t dropped_holds = 0;
    std::int64_t dropped_locked_notes = 0;
};

struct SegmentMapper {
    std::vector<std::int64_t> source_lanes;
    std::vector<std::int64_t> target_lanes;
    std::vector<std::vector<std::int64_t>> cycles;
    std::vector<std::int64_t> offsets;
    std::vector<std::int64_t> occurrences;
    std::int64_t period = 0;

    SegmentMapper(
        const std::vector<std::int64_t>& free_sources,
        const std::vector<std::int64_t>& free_targets
    )
        : source_lanes(free_sources),
          target_lanes(free_targets),
          occurrences(free_sources.size(), 0) {
        if (source_lanes.empty() || target_lanes.empty()) {
            return;
        }

        const auto matrix = build_integer_matrix(
            static_cast<std::int64_t>(source_lanes.size()),
            static_cast<std::int64_t>(target_lanes.size()),
            period
        );
        for (const auto& row : matrix) {
            cycles.push_back(build_balanced_cycle(row));
        }
        offsets = choose_phase_offsets(cycles, matrix,
                                       static_cast<std::int64_t>(
                                           target_lanes.size()
                                       ));
    }

    std::optional<std::int64_t> map(std::int64_t source) {
        const auto source_it = std::find(source_lanes.begin(),
                                         source_lanes.end(), source);
        if (source_it == source_lanes.end() || period == 0) {
            return std::nullopt;
        }

        const auto source_index = static_cast<std::size_t>(
            std::distance(source_lanes.begin(), source_it)
        );
        const auto phase = occurrences[source_index] % period;
        const auto target_index = cycles[source_index][
            static_cast<std::size_t>(
                (phase + offsets[source_index]) % period
            )
        ];
        ++occurrences[source_index];
        return target_lanes[static_cast<std::size_t>(target_index)];
    }
};

static bool gap_less(const Gap& left, const Gap& right) {
    if (left.infinite) {
        return false;
    }
    if (right.infinite) {
        return true;
    }
    return left.value < right.value;
}

static bool gap_equal(const Gap& left, const Gap& right) {
    return left.infinite == right.infinite
        && (left.infinite || left.value == right.value);
}

static std::optional<Rational> nearest_previous_gap(
    const std::vector<std::vector<Rational>>& target_times,
    std::int64_t target,
    const Rational& beat
) {
    if (target_times[static_cast<std::size_t>(target)].empty()) {
        return std::nullopt;
    }
    std::optional<Rational> nearest;
    for (const auto& previous :
         target_times[static_cast<std::size_t>(target)]) {
        const auto gap = beat - previous;
        if (!nearest || gap < *nearest) {
            nearest = gap;
        }
    }
    return nearest;
}

static std::optional<Rational> endbeat_value(const json& note) {
    if (!note.is_object() || !note.contains("endbeat")) {
        return std::nullopt;
    }
    const auto end = beat_value(
        json{{"beat", note["endbeat"]}}
    );
    if (!end) {
        return std::nullopt;
    }
    const auto start = beat_value(note);
    if (!start || !(*start < *end)) {
        return std::nullopt;
    }
    return end;
}

static bool is_hold_note(const json& note) {
    return endbeat_value(note).has_value();
}

static std::vector<std::int64_t> ordered_hold_candidates(
    std::int64_t base_target,
    std::int64_t source,
    const std::vector<std::vector<std::int64_t>>& matrix,
    std::int64_t target_columns
) {
    std::vector<std::int64_t> preferred;
    std::vector<std::int64_t> fallback;
    for (std::int64_t target = 0; target < target_columns; ++target) {
        if (target == base_target) {
            continue;
        }
        if (matrix[source][target] > 0) {
            preferred.push_back(target);
        } else {
            fallback.push_back(target);
        }
    }

    auto by_distance = [base_target](
        std::int64_t left,
        std::int64_t right
    ) {
        const auto left_distance = std::llabs(left - base_target);
        const auto right_distance = std::llabs(right - base_target);
        if (left_distance != right_distance) {
            return left_distance < right_distance;
        }
        return left < right;
    };
    std::sort(preferred.begin(), preferred.end(), by_distance);
    std::sort(fallback.begin(), fallback.end(), by_distance);

    std::vector<std::int64_t> result;
    result.reserve(static_cast<std::size_t>(target_columns));
    result.push_back(base_target);
    result.insert(result.end(), preferred.begin(), preferred.end());
    result.insert(result.end(), fallback.begin(), fallback.end());
    return result;
}

static std::optional<std::int64_t> assign_hold_target(
    std::int64_t source,
    std::int64_t base_target,
    const std::vector<std::vector<std::int64_t>>& matrix,
    const std::vector<bool>& active_targets,
    const std::vector<std::int64_t>& reserved_targets,
    std::int64_t target_columns
) {
    const auto candidates = ordered_hold_candidates(
        base_target, source, matrix, target_columns
    );
    for (const auto target : candidates) {
        if (active_targets[static_cast<std::size_t>(target)]) {
            continue;
        }
        if (std::find(reserved_targets.begin(), reserved_targets.end(),
                      target) != reserved_targets.end()) {
            continue;
        }
        return target;
    }
    return std::nullopt;
}

static bool target_is_locked(
    const std::vector<HoldInfo>& active_holds,
    std::int64_t target
) {
    return std::any_of(
        active_holds.begin(), active_holds.end(),
        [target](const HoldInfo& hold) {
            return hold.target == target;
        }
    );
}

static json rational_to_beat(const Rational& value) {
    if (value.numerator < 0) {
        throw std::invalid_argument("negative beat cannot be serialized");
    }
    const auto integer = value.numerator / value.denominator;
    const auto numerator = value.numerator % value.denominator;
    if (numerator == 0) {
        return json::array({integer, 0, 1});
    }
    return json::array({integer, numerator, value.denominator});
}

static bool trim_hold_before(
    std::vector<json>& ordinary_notes,
    std::vector<HoldInfo>& active_holds,
    std::vector<bool>& active_sources,
    std::vector<bool>& active_targets,
    std::size_t active_index,
    const Rational& conflict_beat
) {
    auto& hold = active_holds[active_index];
    const auto shortened_end = conflict_beat - Rational(1, 4);
    auto& note = ordinary_notes[hold.note_index];
    if (hold.start < shortened_end) {
        note["endbeat"] = rational_to_beat(shortened_end);
    } else {
        note.erase("endbeat");
    }
    active_sources[static_cast<std::size_t>(hold.source)] = false;
    active_targets[static_cast<std::size_t>(hold.target)] = false;
    active_holds.erase(active_holds.begin()
                       + static_cast<std::ptrdiff_t>(active_index));
    return true;
}

static bool trim_hold_on_target(
    std::vector<json>& ordinary_notes,
    std::vector<HoldInfo>& active_holds,
    std::vector<bool>& active_sources,
    std::vector<bool>& active_targets,
    std::int64_t target,
    const Rational& conflict_beat
) {
    for (std::size_t index = 0; index < active_holds.size(); ++index) {
        if (active_holds[index].target == target) {
            return trim_hold_before(
                ordinary_notes, active_holds, active_sources,
                active_targets, index, conflict_beat
            );
        }
    }
    return false;
}

static bool trim_hold_on_source(
    std::vector<json>& ordinary_notes,
    std::vector<HoldInfo>& active_holds,
    std::vector<bool>& active_sources,
    std::vector<bool>& active_targets,
    std::int64_t source,
    const Rational& conflict_beat
) {
    for (std::size_t index = 0; index < active_holds.size(); ++index) {
        if (active_holds[index].source == source) {
            return trim_hold_before(
                ordinary_notes, active_holds, active_sources,
                active_targets, index, conflict_beat
            );
        }
    }
    return false;
}

static HoldProcessStats process_holds(
    std::vector<json>& ordinary_notes,
    const std::vector<std::optional<std::int64_t>>& source_ranks,
    const std::vector<std::int64_t>& source_by_index,
    const std::vector<std::vector<std::int64_t>>& matrix,
    const std::vector<std::vector<std::int64_t>>& cycles,
    const std::vector<std::int64_t>& offsets,
    std::int64_t period,
    std::int64_t source_columns,
    std::int64_t target_columns,
    std::vector<bool>& keep
) {
    std::vector<std::size_t> chronological_indexes;
    for (std::size_t index = 0; index < ordinary_notes.size(); ++index) {
        if (source_ranks[index] && beat_value(ordinary_notes[index])) {
            chronological_indexes.push_back(index);
        }
    }
    std::sort(
        chronological_indexes.begin(), chronological_indexes.end(),
        [&](std::size_t left, std::size_t right) {
            const auto left_beat = *beat_value(ordinary_notes[left]);
            const auto right_beat = *beat_value(ordinary_notes[right]);
            if (left_beat == right_beat) {
                return left < right;
            }
            return left_beat < right_beat;
        }
    );

    std::vector<HoldInfo> active_holds;
    std::vector<bool> active_sources(
        static_cast<std::size_t>(source_columns), false
    );
    std::vector<bool> active_targets(
        static_cast<std::size_t>(target_columns), false
    );
    HoldProcessStats stats;

    std::size_t cursor = 0;
    while (cursor < chronological_indexes.size()) {
        const auto first_index = chronological_indexes[cursor];
        const auto current_beat = *beat_value(ordinary_notes[first_index]);
        std::size_t group_end = cursor + 1;
        while (group_end < chronological_indexes.size()
               && *beat_value(ordinary_notes[chronological_indexes[group_end]])
                   == current_beat) {
            ++group_end;
        }

        active_holds.erase(
            std::remove_if(
                active_holds.begin(), active_holds.end(),
                [&](const HoldInfo& hold) {
                    if (!(hold.end <= current_beat)) {
                        return false;
                    }
                    active_sources[static_cast<std::size_t>(hold.source)] = false;
                    active_targets[static_cast<std::size_t>(hold.target)] = false;
                    return true;
                }
            ),
            active_holds.end()
        );

        std::vector<std::size_t> hold_indexes;
        std::vector<std::size_t> ordinary_indexes;
        for (std::size_t position = cursor; position < group_end; ++position) {
            const auto index = chronological_indexes[position];
            if (is_hold_note(ordinary_notes[index])) {
                hold_indexes.push_back(index);
            } else {
                ordinary_indexes.push_back(index);
            }
        }

        std::vector<std::int64_t> reserved_targets;
        auto release_reserved_target = [&](std::int64_t target) {
            reserved_targets.erase(
                std::remove(reserved_targets.begin(), reserved_targets.end(),
                            target),
                reserved_targets.end()
            );
        };
        for (const auto index : hold_indexes) {
            const auto source = source_by_index[index];
            if (active_sources[static_cast<std::size_t>(source)]) {
                for (const auto& hold : active_holds) {
                    if (hold.source == source) {
                        const auto target = hold.target;
                        const auto same_start = hold.start == current_beat;
                        if (trim_hold_on_source(
                                ordinary_notes, active_holds,
                                active_sources, active_targets,
                                source, current_beat)) {
                            if (!same_start) {
                                release_reserved_target(target);
                            }
                            ++stats.trimmed_holds;
                        }
                        break;
                    }
                }
            }

            const auto phase = *source_ranks[index] % period;
            const auto base_target = cycles[static_cast<std::size_t>(source)][
                static_cast<std::size_t>(
                    (phase + offsets[static_cast<std::size_t>(source)]) % period
                )
            ];
            const auto candidate_order = ordered_hold_candidates(
                base_target, source, matrix, target_columns
            );
            auto target = assign_hold_target(
                source, base_target, matrix, active_targets,
                reserved_targets, target_columns
            );
            if (!target) {
                for (const auto candidate : candidate_order) {
                    if (!target_is_locked(active_holds, candidate)) {
                        continue;
                    }
                    const auto same_start = std::any_of(
                        active_holds.begin(), active_holds.end(),
                        [&](const HoldInfo& hold) {
                            return hold.target == candidate
                                && hold.start == current_beat;
                        }
                    );
                    if (trim_hold_on_target(
                            ordinary_notes, active_holds, active_sources,
                            active_targets, candidate, current_beat)) {
                        if (!same_start) {
                            release_reserved_target(candidate);
                        }
                        ++stats.trimmed_holds;
                        target = assign_hold_target(
                            source, base_target, matrix, active_targets,
                            reserved_targets, target_columns
                        );
                        if (target) {
                            break;
                        }
                    }
                }
            }
            if (!target) {
                for (const auto candidate : reserved_targets) {
                    const auto same_start = std::any_of(
                        active_holds.begin(), active_holds.end(),
                        [&](const HoldInfo& hold) {
                            return hold.target == candidate
                                && hold.start == current_beat;
                        }
                    );
                    if (trim_hold_on_target(
                            ordinary_notes, active_holds, active_sources,
                            active_targets, candidate, current_beat)) {
                        if (!same_start) {
                            release_reserved_target(candidate);
                        }
                        ++stats.trimmed_holds;
                        target = assign_hold_target(
                            source, base_target, matrix, active_targets,
                            reserved_targets, target_columns
                        );
                        if (target) {
                            break;
                        }
                    }
                }
            }
            if (!target) {
                // If every free target is already reserved by a same-beat
                // trimmed hold head, keep the later hold rather than drop it.
                // A duplicate same-beat head is unavoidable when m < n.
                for (const auto candidate : candidate_order) {
                    if (!active_targets[static_cast<std::size_t>(candidate)]) {
                        target = candidate;
                        break;
                    }
                }
            }
            if (!target) {
                keep[index] = false;
                ++stats.dropped_holds;
                continue;
            }

            const auto end = *endbeat_value(ordinary_notes[index]);
            ordinary_notes[index]["column"] = *target;
            reserved_targets.push_back(*target);
            active_sources[static_cast<std::size_t>(source)] = true;
            active_targets[static_cast<std::size_t>(*target)] = true;
            active_holds.push_back({
                index, source, *target, current_beat, end
            });
        }

        std::vector<std::int64_t> free_sources;
        std::vector<std::int64_t> free_targets;
        for (std::int64_t source = 0; source < source_columns; ++source) {
            if (!active_sources[static_cast<std::size_t>(source)]) {
                free_sources.push_back(source);
            }
        }
        for (std::int64_t target = 0; target < target_columns; ++target) {
            if (!active_targets[static_cast<std::size_t>(target)]) {
                free_targets.push_back(target);
            }
        }
        SegmentMapper mapper(free_sources, free_targets);

        for (const auto index : ordinary_indexes) {
            const auto source = source_by_index[index];
            if (active_sources[static_cast<std::size_t>(source)]) {
                keep[index] = false;
                ++stats.dropped_locked_notes;
                continue;
            }
            const auto target = mapper.map(source);
            if (!target) {
                keep[index] = false;
                ++stats.dropped_locked_notes;
                continue;
            }
            ordinary_notes[index]["column"] = *target;
        }

        cursor = group_end;
    }

    return stats;
}

static std::pair<std::int64_t, std::int64_t> repair_short_target_gaps(
    std::vector<json>& ordinary_notes,
    const std::vector<std::int64_t>& source_by_index,
    const std::vector<std::optional<std::int64_t>>& initial_targets,
    const std::vector<std::vector<std::int64_t>>& matrix,
    std::int64_t source_columns,
    std::int64_t target_columns,
    const Rational& minimum_input_gap
) {
    std::vector<std::size_t> chronological_indexes;
    for (std::size_t index = 0; index < ordinary_notes.size(); ++index) {
        if (initial_targets[index] && beat_value(ordinary_notes[index])) {
            chronological_indexes.push_back(index);
        }
    }
    std::sort(
        chronological_indexes.begin(), chronological_indexes.end(),
        [&](std::size_t left, std::size_t right) {
            const auto left_beat = *beat_value(ordinary_notes[left]);
            const auto right_beat = *beat_value(ordinary_notes[right]);
            if (left_beat == right_beat) {
                return left < right;
            }
            return left_beat < right_beat;
        }
    );

    std::vector<std::vector<Rational>> target_times(
        static_cast<std::size_t>(target_columns)
    );
    const auto minimum_target_gap =
        minimum_input_gap * target_columns / source_columns;
    std::int64_t changed = 0;
    std::int64_t unresolved = 0;

    for (const auto index : chronological_indexes) {
        const auto beat = *beat_value(ordinary_notes[index]);
        const auto source = source_by_index[index];
        const auto initial_target = *initial_targets[index];
        std::vector<std::int64_t> allowed_targets;
        for (std::int64_t target = 0; target < target_columns; ++target) {
            if (matrix[source][target] > 0) {
                allowed_targets.push_back(target);
            }
        }
        if (allowed_targets.empty()) {
            for (std::int64_t target = 0; target < target_columns; ++target) {
                allowed_targets.push_back(target);
            }
        }

        auto gap_for = [&](std::int64_t target) -> Gap {
            const auto previous = nearest_previous_gap(target_times, target, beat);
            return previous ? Gap{false, *previous} : Gap{true, {}};
        };
        auto is_safe = [&](const Gap& gap) {
            return gap.infinite
                || (Rational(0, 1) < gap.value
                    && minimum_target_gap <= gap.value);
        };

        const auto initial_gap = gap_for(initial_target);
        auto chosen_target = initial_target;
        if (!is_safe(initial_gap)) {
            struct Candidate {
                Gap gap;
                std::int64_t target;
            };
            std::vector<Candidate> candidates;
            for (const auto target : allowed_targets) {
                candidates.push_back({gap_for(target), target});
            }

            auto choose_safe = [&](const std::vector<Candidate>& values)
                -> std::optional<std::int64_t> {
                std::vector<Candidate> safe_values;
                for (const auto& item : values) {
                    if (is_safe(item.gap)) {
                        safe_values.push_back(item);
                    }
                }
                if (safe_values.empty()) {
                    return std::nullopt;
                }
                std::sort(
                    safe_values.begin(), safe_values.end(),
                    [&](const Candidate& left, const Candidate& right) {
                        if (!gap_equal(left.gap, right.gap)) {
                            return gap_less(left.gap, right.gap);
                        }
                        const bool left_is_initial = left.target == initial_target;
                        const bool right_is_initial = right.target == initial_target;
                        if (left_is_initial != right_is_initial) {
                            return left_is_initial;
                        }
                        const auto left_distance =
                            std::llabs(left.target - initial_target);
                        const auto right_distance =
                            std::llabs(right.target - initial_target);
                        if (left_distance != right_distance) {
                            return left_distance < right_distance;
                        }
                        return left.target < right.target;
                    }
                );
                return safe_values.front().target;
            };

            auto safe = choose_safe(candidates);
            if (!safe) {
                std::vector<Candidate> all_candidates;
                for (std::int64_t target = 0; target < target_columns; ++target) {
                    if (std::find(allowed_targets.begin(), allowed_targets.end(),
                                  target) == allowed_targets.end()) {
                        all_candidates.push_back({gap_for(target), target});
                    }
                }
                safe = choose_safe(all_candidates);
                if (!safe) {
                    candidates.insert(candidates.end(),
                                      all_candidates.begin(), all_candidates.end());
                    std::sort(
                        candidates.begin(), candidates.end(),
                        [&](const Candidate& left, const Candidate& right) {
                            if (!gap_equal(left.gap, right.gap)) {
                                return gap_less(right.gap, left.gap);
                            }
                            const bool left_is_initial =
                                left.target == initial_target;
                            const bool right_is_initial =
                                right.target == initial_target;
                            if (left_is_initial != right_is_initial) {
                                return left_is_initial;
                            }
                            const auto left_distance =
                                std::llabs(left.target - initial_target);
                            const auto right_distance =
                                std::llabs(right.target - initial_target);
                            if (left_distance != right_distance) {
                                return left_distance < right_distance;
                            }
                            return left.target < right.target;
                        }
                    );
                    chosen_target = candidates.front().target;
                    ++unresolved;
                } else {
                    chosen_target = *safe;
                }
            } else {
                chosen_target = *safe;
            }
            if (chosen_target != initial_target) {
                ++changed;
            }
        }

        ordinary_notes[index]["column"] = chosen_target;
        target_times[static_cast<std::size_t>(chosen_target)].push_back(beat);
    }
    return {changed, unresolved};
}

static std::pair<std::int64_t, std::int64_t> repair_hold_results(
    std::vector<json>& ordinary_notes,
    const std::vector<std::int64_t>& source_by_index,
    std::vector<bool>& keep,
    std::int64_t source_columns,
    std::int64_t target_columns,
    const Rational& minimum_input_gap
) {
    std::vector<std::size_t> chronological_indexes;
    for (std::size_t index = 0; index < ordinary_notes.size(); ++index) {
        if (keep[index] && beat_value(ordinary_notes[index])) {
            chronological_indexes.push_back(index);
        }
    }
    std::sort(
        chronological_indexes.begin(), chronological_indexes.end(),
        [&](std::size_t left, std::size_t right) {
            const auto left_beat = *beat_value(ordinary_notes[left]);
            const auto right_beat = *beat_value(ordinary_notes[right]);
            if (left_beat == right_beat) {
                return left < right;
            }
            return left_beat < right_beat;
        }
    );

    std::vector<HoldInfo> active_holds;
    std::vector<std::vector<Rational>> target_times(
        static_cast<std::size_t>(target_columns)
    );
    const auto minimum_target_gap =
        minimum_input_gap * target_columns / source_columns;
    std::int64_t changed = 0;
    std::int64_t dropped = 0;

    for (const auto index : chronological_indexes) {
        const auto beat = *beat_value(ordinary_notes[index]);
        active_holds.erase(
            std::remove_if(
                active_holds.begin(), active_holds.end(),
                [&](const HoldInfo& hold) {
                    return hold.end <= beat;
                }
            ),
            active_holds.end()
        );

        const auto current_target =
            ordinary_notes[index]["column"].get<std::int64_t>();
        if (is_hold_note(ordinary_notes[index])) {
            active_holds.push_back({
                index,
                source_by_index[index],
                current_target,
                beat,
                *endbeat_value(ordinary_notes[index])
            });
            target_times[static_cast<std::size_t>(current_target)].push_back(
                beat
            );
            continue;
        }

        auto target_locked = [&](std::int64_t target) {
            return target_is_locked(active_holds, target);
        };
        auto gap_for = [&](std::int64_t target) -> Gap {
            if (target_locked(target)) {
                return {false, Rational(0, 1)};
            }
            const auto previous = nearest_previous_gap(
                target_times, target, beat
            );
            return previous ? Gap{false, *previous} : Gap{true, {}};
        };
        auto is_safe = [&](const Gap& gap) {
            return gap.infinite
                || (Rational(0, 1) < gap.value
                    && minimum_target_gap <= gap.value);
        };

        std::vector<std::pair<Gap, std::int64_t>> candidates;
        candidates.reserve(static_cast<std::size_t>(target_columns));
        for (std::int64_t target = 0; target < target_columns; ++target) {
            candidates.push_back({gap_for(target), target});
        }
        const auto initial_gap = gap_for(current_target);
        if (!is_safe(initial_gap)) {
            std::vector<std::pair<Gap, std::int64_t>> safe;
            for (const auto& candidate : candidates) {
                if (is_safe(candidate.first)) {
                    safe.push_back(candidate);
                }
            }
            if (!safe.empty()) {
                std::sort(
                    safe.begin(), safe.end(),
                    [&](const auto& left, const auto& right) {
                        if (!gap_equal(left.first, right.first)) {
                            return gap_less(left.first, right.first);
                        }
                        const auto left_distance =
                            std::llabs(left.second - current_target);
                        const auto right_distance =
                            std::llabs(right.second - current_target);
                        if (left_distance != right_distance) {
                            return left_distance < right_distance;
                        }
                        return left.second < right.second;
                    }
                );
                const auto selected = safe.front().second;
                if (selected != current_target) {
                    ordinary_notes[index]["column"] = selected;
                    ++changed;
                }
            } else {
                candidates.erase(
                    std::remove_if(
                        candidates.begin(), candidates.end(),
                        [](const auto& item) {
                            return item.first.infinite;
                        }
                    ),
                    candidates.end()
                );
                if (candidates.empty()) {
                    keep[index] = false;
                    ++dropped;
                    continue;
                }
                std::sort(
                    candidates.begin(), candidates.end(),
                    [&](const auto& left, const auto& right) {
                        if (!gap_equal(left.first, right.first)) {
                            return gap_less(right.first, left.first);
                        }
                        const auto left_distance =
                            std::llabs(left.second - current_target);
                        const auto right_distance =
                            std::llabs(right.second - current_target);
                        if (left_distance != right_distance) {
                            return left_distance < right_distance;
                        }
                        return left.second < right.second;
                    }
                );
                const auto selected = candidates.front().second;
                if (selected != current_target) {
                    ordinary_notes[index]["column"] = selected;
                    ++changed;
                }
            }
        }

        if (keep[index]) {
            const auto final_target =
                ordinary_notes[index]["column"].get<std::int64_t>();
            target_times[static_cast<std::size_t>(final_target)].push_back(
                beat
            );
        }
    }
    return {changed, dropped};
}

static void process_mc(
    const std::filesystem::path& input_path,
    std::int64_t target_columns,
    std::optional<std::int64_t> source_columns_override
) {
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open input file");
    }
    const auto data = json::parse(input);
    const auto notes_json = data.value("note", json::array());
    if (!notes_json.is_array() || notes_json.empty()) {
        std::cout << "Warning: no note list found\n";
        return;
    }

    std::vector<json> sound_notes;
    std::vector<json> ordinary_notes;
    for (const auto& note : notes_json) {
        if (note.is_object() && note.value("type", 0) == 1) {
            sound_notes.push_back(note);
        } else {
            ordinary_notes.push_back(note);
        }
    }

    const auto source_columns = source_columns_override.value_or(
        read_source_columns(data, ordinary_notes)
    );
    if (source_columns <= 0) {
        throw std::invalid_argument("Source lane count must be positive");
    }
    if (target_columns <= 0) {
        throw std::invalid_argument("Target lane count must be positive");
    }

    std::int64_t period = 0;
    const auto matrix = build_integer_matrix(
        source_columns, target_columns, period
    );
    std::vector<std::vector<std::int64_t>> cycles;
    for (const auto& row : matrix) {
        cycles.push_back(build_balanced_cycle(row));
    }
    const auto offsets = choose_phase_offsets(cycles, matrix, target_columns);

    std::vector<std::vector<std::size_t>> indexes_by_source(
        static_cast<std::size_t>(source_columns)
    );
    for (std::size_t index = 0; index < ordinary_notes.size(); ++index) {
        if (ordinary_notes[index].is_object()
            && ordinary_notes[index].contains("column")
            && is_valid_column(ordinary_notes[index]["column"], source_columns)) {
            indexes_by_source[
                static_cast<std::size_t>(
                    ordinary_notes[index]["column"].get<std::int64_t>()
                )
            ].push_back(index);
        }
    }

    std::vector<std::optional<std::int64_t>> source_ranks(ordinary_notes.size());
    std::vector<std::optional<Rational>> previous_beats(
        static_cast<std::size_t>(source_columns)
    );
    std::vector<Rational> all_positive_gaps;
    for (auto& indexes : indexes_by_source) {
        std::sort(
            indexes.begin(), indexes.end(),
            [&](std::size_t left, std::size_t right) {
                const auto left_beat = beat_value(ordinary_notes[left]);
                const auto right_beat = beat_value(ordinary_notes[right]);
                if (left_beat.has_value() != right_beat.has_value()) {
                    return !left_beat.has_value();
                }
                if (left_beat && right_beat && !(*left_beat == *right_beat)) {
                    return *left_beat < *right_beat;
                }
                return left < right;
            }
        );
        for (std::size_t rank = 0; rank < indexes.size(); ++rank) {
            const auto index = indexes[rank];
            source_ranks[index] = static_cast<std::int64_t>(rank);
            const auto current = beat_value(ordinary_notes[index]);
            if (current && previous_beats[
                    static_cast<std::size_t>(
                        ordinary_notes[index]["column"].get<std::int64_t>()
                    )
                ]) {
                const auto gap = *current - *previous_beats[
                    static_cast<std::size_t>(
                        ordinary_notes[index]["column"].get<std::int64_t>()
                    )
                ];
                if (Rational(0, 1) < gap) {
                    all_positive_gaps.push_back(gap);
                }
            }
            if (current) {
                previous_beats[
                    static_cast<std::size_t>(
                        ordinary_notes[index]["column"].get<std::int64_t>()
                    )
                ] = current;
            }
        }
    }
    Rational minimum_input_gap(0, 1);
    if (!all_positive_gaps.empty()) {
        minimum_input_gap = *std::min_element(
            all_positive_gaps.begin(), all_positive_gaps.end()
        );
    }

    std::vector<std::int64_t> source_by_index(ordinary_notes.size(), 0);
    std::vector<std::optional<std::int64_t>> initial_targets(
        ordinary_notes.size()
    );
    std::vector<bool> keep(ordinary_notes.size(), true);
    std::int64_t invalid_columns = 0;
    const bool has_holds = std::any_of(
        ordinary_notes.begin(), ordinary_notes.end(),
        [](const json& note) {
            return is_hold_note(note);
        }
    );

    std::int64_t trimmed_holds = 0;
    std::int64_t dropped_holds = 0;
    std::int64_t dropped_locked_notes = 0;
    std::int64_t repaired = 0;
    std::int64_t unresolved = 0;
    for (std::size_t index = 0; index < ordinary_notes.size(); ++index) {
        if (!ordinary_notes[index].is_object()
            || !ordinary_notes[index].contains("column")
            || !is_valid_column(ordinary_notes[index]["column"], source_columns)) {
            std::cout << "Warning: skipped invalid column value ";
            if (ordinary_notes[index].is_object()
                && ordinary_notes[index].contains("column")) {
                std::cout << ordinary_notes[index]["column"].dump();
            } else {
                std::cout << "null";
            }
            std::cout << '\n';
            ++invalid_columns;
            continue;
        }
        const auto source = ordinary_notes[index]["column"].get<std::int64_t>();
        source_by_index[index] = source;
        if (!source_ranks[index]) {
            keep[index] = false;
            continue;
        }

        const auto phase = *source_ranks[index] % period;
        const auto& cycle = cycles[static_cast<std::size_t>(source)];
        const auto target = cycle[static_cast<std::size_t>(
            (phase + offsets[static_cast<std::size_t>(source)]) % period
        )];
        initial_targets[index] = target;
    }

    if (has_holds) {
        const auto dropped = process_holds(
            ordinary_notes, source_ranks, source_by_index, matrix,
            cycles, offsets, period, source_columns, target_columns, keep
        );
        trimmed_holds = dropped.trimmed_holds;
        dropped_holds = dropped.dropped_holds;
        dropped_locked_notes = dropped.dropped_locked_notes;

        const auto hold_repair = repair_hold_results(
            ordinary_notes, source_by_index, keep, source_columns,
            target_columns, minimum_input_gap
        );
        repaired = hold_repair.first;
        dropped_locked_notes += hold_repair.second;
    } else {
        for (std::size_t index = 0; index < ordinary_notes.size(); ++index) {
            if (!initial_targets[index]) {
                continue;
            }
            ordinary_notes[index]["column"] = *initial_targets[index];
        }
        const auto repair = repair_short_target_gaps(
            ordinary_notes, source_by_index, initial_targets, matrix,
            source_columns, target_columns, minimum_input_gap
        );
        repaired = repair.first;
        unresolved = repair.second;
    }

    std::vector<std::pair<Rational, std::int64_t>> mapped_locations;
    for (std::size_t note_index = 0;
         note_index < ordinary_notes.size();
         ++note_index) {
        if (!keep[note_index]) {
            continue;
        }
        const auto& note = ordinary_notes[note_index];
        const auto beat = beat_value(note);
        if (!beat || !note.is_object() || !note.contains("column")
            || !note["column"].is_number_integer()) {
            continue;
        }
        mapped_locations.emplace_back(*beat, note["column"].get<std::int64_t>());
    }
    std::sort(
        mapped_locations.begin(), mapped_locations.end(),
        [](const auto& left, const auto& right) {
            return left.first == right.first
                ? left.second < right.second
                : left.first < right.first;
        }
    );
    std::int64_t collisions = 0;
    for (std::size_t index = 0; index < mapped_locations.size();) {
        std::size_t end = index + 1;
        while (end < mapped_locations.size()
               && mapped_locations[end].first == mapped_locations[index].first
               && mapped_locations[end].second == mapped_locations[index].second) {
            ++end;
        }
        if (end - index > 1) {
            ++collisions;
        }
        index = end;
    }

    json output = data;
    output["note"] = json::array();
    for (std::size_t note_index = 0;
         note_index < ordinary_notes.size();
         ++note_index) {
        if (keep[note_index]) {
            output["note"].push_back(ordinary_notes[note_index]);
        }
    }
    for (const auto& note : sound_notes) {
        output["note"].push_back(note);
    }
    try {
        output.at("meta").at("mode_ext")["column"] = target_columns;
    } catch (...) {
        std::cout
            << "Warning: meta or mode_ext is incomplete; "
               "could not update target column count\n";
    }

    auto input_text = input_path.u8string();
    const auto separator = input_text.find_last_of("/\\");
    const auto filename_start = separator == std::string::npos
        ? 0
        : separator + 1;
    const auto dot = input_text.find_last_of('.');
    const auto extension_start = dot == std::string::npos || dot < filename_start
        ? input_text.size()
        : dot;
    const auto output_text = input_text.substr(0, extension_start)
        + "_to" + std::to_string(target_columns) + "K"
        + input_text.substr(extension_start);
    const auto output_path = std::filesystem::u8path(output_text);
    std::ofstream output_file(output_path, std::ios::binary);
    if (!output_file) {
        throw std::runtime_error("could not open output file");
    }
    output_file << output.dump(2);

    std::cout << "Processed " << source_columns << "K -> "
              << target_columns << "K; period=" << period
              << "; version=" << PRONTOM_VERSION
              << "; output: " << output_text << '\n';
    if (invalid_columns) {
        std::cout << "Warning: skipped " << invalid_columns
                  << " invalid note columns\n";
    }
    if (dropped_holds || dropped_locked_notes) {
        std::cout << "Dropped " << dropped_holds + dropped_locked_notes
                  << " note(s) because a frozen hold occupied the required "
                     "source or target lane.\n";
    }
    if (trimmed_holds) {
        std::cout << "Trimmed " << trimmed_holds
                  << " earlier hold(s) at a later conflict by 1/4 beat.\n";
    }
    if (collisions) {
        std::cout << "Warning: detected " << collisions
                  << " same-beat target-column collision(s). "
                     "No notes were silently removed.\n";
    }
    if (repaired) {
        std::cout << "Repaired " << repaired
                  << " target-column assignment(s) with expansion ratio "
                  << target_columns << "/" << source_columns
                  << " (input minimum gap " << rational_to_string(minimum_input_gap)
                  << ").\n";
    }
    if (unresolved) {
        std::cout << "Warning: " << unresolved
                  << " target-column assignment(s) still have no possible "
                     "spacing-preserving lane.\n";
    }
}

static std::int64_t parse_positive_integer(const std::string& text,
                                           const char* message) {
    try {
        std::size_t position = 0;
        const auto value = std::stoll(text, &position, 10);
        if (position != text.size()) {
            throw std::invalid_argument("not an integer");
        }
        if (value <= 0) {
            throw std::invalid_argument(message);
        }
        return value;
    } catch (...) {
        throw std::invalid_argument(message);
    }
}

#ifdef _WIN32
static std::string utf8_from_wide(const wchar_t* value) {
    const auto size = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr
    );
    if (size <= 0) {
        throw std::runtime_error("could not convert command line");
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr
    );
    return result;
}
#endif

static int run(int argc, const char* const* argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "prontom " << PRONTOM_VERSION << '\n';
        return 0;
    }
    if (argc < 2) {
        std::cout << "Usage: prontom.exe input.mc\n"
                     "Optional non-interactive form: prontom.exe "
                     "input.mc target_columns [source_columns]\n";
        return 1;
    }

    const std::string input_text = argv[1];
    const auto input_path = std::filesystem::u8path(input_text);
    if (!std::filesystem::exists(input_path)) {
        std::cout << "Input file does not exist: " << input_text << '\n';
        return 1;
    }

    try {
        std::string target_text;
        std::optional<std::int64_t> source_columns;
        if (argc == 3) {
            target_text = argv[2];
        } else if (argc == 4) {
            target_text = argv[2];
            source_columns = parse_positive_integer(
                argv[3], "Source lane count must be an integer"
            );
        } else if (argc == 2) {
            if (!std::getline(std::cin, target_text)) {
                throw std::invalid_argument(
                    "Target lane count is required. Use: prontom.exe "
                    "input.mc target_columns"
                );
            }
        } else {
            std::cout << "Usage: prontom.exe input.mc\n"
                         "Optional non-interactive form: prontom.exe "
                         "input.mc target_columns [source_columns]\n";
            return 1;
        }
        const auto target_columns = parse_positive_integer(
            target_text, "Target lane count must be an integer"
        );
        process_mc(input_path, target_columns, source_columns);
        return 0;
    } catch (const std::exception& error) {
        std::cout << "Error: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(utf8_from_wide(argv[index]));
    }
    std::vector<const char*> pointers;
    pointers.reserve(arguments.size());
    for (const auto& argument : arguments) {
        pointers.push_back(argument.c_str());
    }
    return run(argc, pointers.data());
}
#else
int main(int argc, char* argv[]) {
    return run(argc, argv);
}
#endif
