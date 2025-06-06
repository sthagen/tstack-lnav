/**
 * Copyright (c) 2014, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file readline_highlighters.cc
 */

#include "readline_highlighters.hh"

#include "base/attr_line.builder.hh"
#include "base/snippet_highlighters.hh"
#include "base/string_util.hh"
#include "config.h"
#include "pcrepp/pcre2pp.hh"
#include "shlex.hh"
#include "sql_help.hh"
#include "sql_util.hh"
#include "textfile_highlighters.hh"
#include "view_curses.hh"

static bool
is_bracket(const std::string& str, int index, bool is_lit)
{
    if (is_lit && str[index - 1] == '\\') {
        return true;
    }
    if (!is_lit && str[index - 1] != '\\') {
        return true;
    }
    return false;
}

static void
find_matching_bracket(
    attr_line_t& al, int x, line_range sub, char left, char right)
{
    const auto is_lit = left == 'Q';
    attr_line_builder alb(al);
    const auto& line = al.get_string();
    int depth = 0;

    if (x < sub.lr_start || x > sub.lr_end) {
        return;
    }

    if (line[x] == right && is_bracket(line, x, is_lit)) {
        for (int lpc = x - 1; lpc >= sub.lr_start; lpc--) {
            if (line[lpc] == right && is_bracket(line, lpc, is_lit)) {
                depth += 1;
            } else if (line[lpc] == left && is_bracket(line, lpc, is_lit)) {
                if (depth == 0) {
                    alb.overlay_attr_for_char(
                        lpc,
                        VC_STYLE.value(text_attrs ::with_styles(
                            text_attrs::style::bold,
                            text_attrs::style::reverse)));
                    alb.overlay_attr_for_char(lpc,
                                              VC_ROLE.value(role_t::VCR_OK));
                    break;
                }
                depth -= 1;
            }
        }
    }

    if (line[x] == left && is_bracket(line, x, is_lit)) {
        for (auto lpc = x + 1; lpc < sub.lr_end; lpc++) {
            if (line[lpc] == left && is_bracket(line, lpc, is_lit)) {
                depth += 1;
            } else if (line[lpc] == right && is_bracket(line, lpc, is_lit)) {
                if (depth == 0) {
                    alb.overlay_attr_for_char(
                        lpc,
                        VC_STYLE.value(text_attrs ::with_styles(
                            text_attrs::style::bold,
                            text_attrs::style::reverse)));
                    alb.overlay_attr_for_char(lpc,
                                              VC_ROLE.value(role_t::VCR_OK));
                    break;
                }
                depth -= 1;
            }
        }
    }

    std::optional<int> first_left;

    depth = 0;

    for (auto lpc = sub.lr_start; lpc < sub.lr_end; lpc++) {
        if (line[lpc] == left && is_bracket(line, lpc, is_lit)) {
            depth += 1;
            if (!first_left) {
                first_left = lpc;
            }
        } else if (line[lpc] == right && is_bracket(line, lpc, is_lit)) {
            if (depth > 0) {
                depth -= 1;
            } else {
                auto lr = line_range(is_lit ? lpc - 1 : lpc, lpc + 1);
                alb.overlay_attr(
                    lr,
                    VC_STYLE.value(text_attrs ::with_styles(
                        text_attrs::style::bold, text_attrs::style::reverse)));
                alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_ERROR));
            }
        }
    }

    if (depth > 0) {
        auto lr
            = line_range(is_lit ? first_left.value() - 1 : first_left.value(),
                         first_left.value() + 1);
        alb.overlay_attr(
            lr,
            VC_STYLE.value(text_attrs ::with_styles(
                text_attrs::style::bold, text_attrs::style::reverse)));
        alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_ERROR));
    }
}

void
readline_regex_highlighter(attr_line_t& al, std::optional<int> x)
{
    lnav::snippets::regex_highlighter(
        al, x, line_range{1, (int) al.get_string().size()});
}

static highlight_map_t
get_hl_map()
{
    highlight_map_t retval;

    setup_highlights(retval);
    return retval;
}

void
readline_command_highlighter_int(attr_line_t& al,
                                 std::optional<int> x,
                                 line_range sub)
{
    static const auto TEXT_HIGHLIGHTERS = get_hl_map();
    static const auto RE_PREFIXES = lnav::pcre2pp::code::from_const(
        R"(^:(filter-in|filter-out|delete-filter|enable-filter|disable-filter|highlight|clear-highlight|create-search-table\s+[^\s]+\s+))");
    static const auto SH_PREFIXES = lnav::pcre2pp::code::from_const(
        "^:(eval|open|append-to|write-to|write-csv-to|write-json-to)");
    static const auto SQL_PREFIXES
        = lnav::pcre2pp::code::from_const("^:(filter-expr|mark-expr)");
    static const auto MD_PREFIXES
        = lnav::pcre2pp::code::from_const("^:comment");
    static const auto IDENT_PREFIXES
        = lnav::pcre2pp::code::from_const("^:(tag|untag|delete-tags)");
    static const auto COLOR_PREFIXES
        = lnav::pcre2pp::code::from_const("^:(config)");
    static const auto COLOR_RE = lnav::pcre2pp::code::from_const(
        "(#(?:[a-fA-F0-9]{6}|[a-fA-F0-9]{3}))");

    attr_line_builder alb(al);
    const auto& line = al.get_string();
    auto in_frag
        = string_fragment::from_str_range(line, sub.lr_start, sub.lr_end);
    size_t ws_index;

    ws_index = line.find(' ', sub.lr_start);
    auto command = line.substr(sub.lr_start, ws_index);
    if (ws_index != std::string::npos) {
        auto has_prefix = al.al_string[sub.lr_start] == ':';
        alb.overlay_attr(
            line_range(sub.lr_start + (has_prefix ? 1 : 0), ws_index),
            VC_ROLE.value(role_t::VCR_KEYWORD));

        if (RE_PREFIXES.find_in(in_frag).ignore_error()) {
            lnav::snippets::regex_highlighter(
                al, x, line_range{(int) ws_index, sub.lr_end});
        }
        if (SH_PREFIXES.find_in(in_frag).ignore_error()) {
            readline_shlex_highlighter_int(
                al, x, line_range{(int) ws_index, sub.lr_end});
        }
        if (SQL_PREFIXES.find_in(in_frag).ignore_error()) {
            readline_sqlite_highlighter_int(
                al, x, line_range{(int) ws_index, sub.lr_end});
        }
    }
    if (COLOR_PREFIXES.find_in(in_frag).ignore_error()) {
        COLOR_RE.capture_from(in_frag).for_each(
            [&alb](lnav::pcre2pp::match_data& md) {
                styling::color_unit::from_str(md[0].value())
                    .then([&](const auto& rgb_fg) {
                        auto color
                            = view_colors::singleton().match_color(rgb_fg);
                        auto ta = text_attrs::with_bold();
                        ta.ta_fg_color = color;
                        alb.overlay_attr(to_line_range(md[0].value()),
                                         VC_STYLE.value(ta));
                    });
            });
    }
    if (IDENT_PREFIXES.find_in(in_frag).ignore_error()
        && ws_index != std::string::npos)
    {
        ssize_t start = ws_index, last;

        do {
            for (; start < sub.length() && isspace(line[start]); start++)
                ;
            for (last = start; last < sub.length() && !isspace(line[last]);
                 last++)
                ;
            line_range lr{(int) start, (int) last};

            if (x && lr.length() > 0 && !lr.contains(x.value())
                && !lr.contains(x.value() - 1))
            {
                std::string value(lr.substr(line), lr.sublen(line));

                if ((command == ":tag" || command == ":untag"
                     || command == ":delete-tags")
                    && !startswith(value, "#"))
                {
                    value = "#" + value;
                }
                alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_IDENTIFIER));
            }

            start = last;
        } while (start < sub.length());
    }
    if (MD_PREFIXES.find_in(in_frag).ignore_error()) {
        for (const auto& [src, hl] : TEXT_HIGHLIGHTERS) {
            if (!hl.applies_to_format(text_format_t::TF_MARKDOWN)) {
                continue;
            }

            hl.annotate(al, sub.lr_start + command.length());
        }
    }
}

void
readline_command_highlighter(attr_line_t& al, std::optional<int> x)
{
    readline_command_highlighter_int(
        al, x, line_range{0, (int) al.get_string().length()});
}

void
readline_sqlite_highlighter_int(attr_line_t& al,
                                std::optional<int> x,
                                line_range sub)
{
    static const char* brackets[] = {
        "[]",
        "()",
        "{}",
    };

    attr_line_builder alb(al);
    const auto& line = al.get_string();

    auto anno_sql = al.subline(sub.lr_start, sub.length());
    anno_sql.get_attrs().clear();
    annotate_sql_statement(anno_sql);

    for (const auto& attr : anno_sql.al_attrs) {
        auto lr = line_range{
            sub.lr_start + attr.sa_range.lr_start,
            sub.lr_start + attr.sa_range.lr_end,
        };
        if (attr.sa_type == &SQL_COMMAND_ATTR
            || attr.sa_type == &SQL_KEYWORD_ATTR
            || attr.sa_type == &lnav::sql::PRQL_KEYWORD_ATTR
            || attr.sa_type == &lnav::sql::PRQL_TRANSFORM_ATTR)
        {
            alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_KEYWORD));
        } else if (attr.sa_type == &SQL_IDENTIFIER_ATTR
                   || attr.sa_type == &lnav::sql::PRQL_FQID_ATTR)
        {
            if (!x
                || (x && !attr.sa_range.contains(x.value())
                    && attr.sa_range.lr_end != x.value()))
            {
                alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_IDENTIFIER));
            }
        } else if (attr.sa_type == &SQL_FUNCTION_ATTR) {
            alb.overlay_attr(
                line_range{lr.lr_start, (int) line.find('(', lr.lr_start)},
                VC_ROLE.value(role_t::VCR_SYMBOL));
        } else if (attr.sa_type == &SQL_NUMBER_ATTR
                   || attr.sa_type == &lnav::sql::PRQL_NUMBER_ATTR)
        {
            alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_NUMBER));
        } else if (attr.sa_type == &SQL_HEX_LIT_ATTR) {
            if (lr.length() > 1 && al.al_string[lr.lr_end - 1] == '\'') {
                alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_STRING));
            } else {
                alb.overlay_attr_for_char(
                    lr.lr_start, VC_STYLE.value(text_attrs::with_reverse()));
                alb.overlay_attr_for_char(lr.lr_start,
                                          VC_ROLE.value(role_t::VCR_ERROR));
            }
        } else if (attr.sa_type == &SQL_STRING_ATTR) {
            if (lr.length() > 1 && al.al_string[lr.lr_end - 1] == '\'') {
                alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_STRING));
            } else {
                alb.overlay_attr_for_char(
                    lr.lr_start, VC_STYLE.value(text_attrs::with_reverse()));
                alb.overlay_attr_for_char(lr.lr_start,
                                          VC_ROLE.value(role_t::VCR_ERROR));
            }
        } else if (attr.sa_type == &lnav::sql::PRQL_STRING_ATTR) {
            alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_STRING));
        } else if (attr.sa_type == &SQL_OPERATOR_ATTR
                   || attr.sa_type == &lnav::sql::PRQL_OPERATOR_ATTR)
        {
            alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_SYMBOL));
        } else if (attr.sa_type == &SQL_COMMENT_ATTR
                   || attr.sa_type == &lnav::sql::PRQL_COMMENT_ATTR)
        {
            alb.overlay_attr(lr, VC_ROLE.value(role_t::VCR_COMMENT));
        }
    }

    for (const auto& bracket : brackets) {
        find_matching_bracket(
            al, x.value_or(al.length()), sub, bracket[0], bracket[1]);
    }
}

void
readline_sqlite_highlighter(attr_line_t& al, std::optional<int> x)
{
    readline_sqlite_highlighter_int(
        al, x, line_range{0, (int) al.get_string().length()});
}

void
readline_shlex_highlighter_int(attr_line_t& al,
                               std::optional<int> x,
                               line_range sub)
{
    attr_line_builder alb(al);
    const auto& str = al.get_string();
    std::optional<int> quote_start;
    shlex lexer(string_fragment{al.al_string.data(), sub.lr_start, sub.lr_end});
    bool done = false;

    while (!done) {
        auto tokenize_res = lexer.tokenize();
        if (tokenize_res.isErr()) {
            auto te = tokenize_res.unwrapErr();

            alb.overlay_attr(line_range(sub.lr_start + te.te_source.sf_begin,
                                        sub.lr_start + te.te_source.sf_end),
                             VC_STYLE.value(text_attrs::with_reverse()));
            alb.overlay_attr(line_range(sub.lr_start + te.te_source.sf_begin,
                                        sub.lr_start + te.te_source.sf_end),
                             VC_ROLE.value(role_t::VCR_ERROR));
            return;
        }

        auto token = tokenize_res.unwrap();
        switch (token.tr_token) {
            case shlex_token_t::eof:
                done = true;
                break;
            case shlex_token_t::tilde:
            case shlex_token_t::escape:
                alb.overlay_attr(
                    line_range(sub.lr_start + token.tr_frag.sf_begin,
                               sub.lr_start + token.tr_frag.sf_end),
                    VC_ROLE.value(role_t::VCR_SYMBOL));
                break;
            case shlex_token_t::double_quote_start:
            case shlex_token_t::single_quote_start:
                quote_start = sub.lr_start + token.tr_frag.sf_begin;
                break;
            case shlex_token_t::double_quote_end:
            case shlex_token_t::single_quote_end:
                alb.overlay_attr(
                    line_range(quote_start.value(),
                               sub.lr_start + token.tr_frag.sf_end),
                    VC_ROLE.value(role_t::VCR_STRING));
                quote_start = std::nullopt;
                break;
            case shlex_token_t::variable_ref:
            case shlex_token_t::quoted_variable_ref: {
                int extra = token.tr_token == shlex_token_t::variable_ref ? 0
                                                                          : 1;
                auto ident = str.substr(
                    sub.lr_start + token.tr_frag.sf_begin + 1 + extra,
                    token.tr_frag.length() - 1 - extra * 2);
                alb.overlay_attr(
                    line_range(
                        sub.lr_start + token.tr_frag.sf_begin,
                        sub.lr_start + token.tr_frag.sf_begin + 1 + extra),
                    VC_ROLE.value(role_t::VCR_SYMBOL));
                alb.overlay_attr(
                    line_range(
                        sub.lr_start + token.tr_frag.sf_begin + 1 + extra,
                        sub.lr_start + token.tr_frag.sf_end - extra),
                    VC_ROLE.value(x == sub.lr_start + token.tr_frag.sf_end
                                          || (token.tr_frag.sf_begin <= x
                                              && x < token.tr_frag.sf_end)
                                      ? role_t::VCR_SYMBOL
                                      : role_t::VCR_IDENTIFIER));
                if (extra) {
                    alb.overlay_attr_for_char(
                        sub.lr_start + token.tr_frag.sf_end - 1,
                        VC_ROLE.value(role_t::VCR_SYMBOL));
                }
                break;
            }
            case shlex_token_t::whitespace:
                break;
        }
    }

    if (quote_start) {
        alb.overlay_attr_for_char(quote_start.value(),
                                  VC_ROLE.value(role_t::VCR_ERROR));
    }
}

void
readline_shlex_highlighter(attr_line_t& al, std::optional<int> x)
{
    readline_shlex_highlighter_int(
        al, x, line_range{0, (int) al.al_string.length()});
}

static void
readline_lnav_highlighter_int(attr_line_t& al,
                              std::optional<int> x,
                              line_range sub)
{
    switch (al.al_string[sub.lr_start]) {
        case ':':
            readline_command_highlighter_int(al, x, sub);
            break;
        case ';':
            readline_sqlite_highlighter_int(al,
                                            x,
                                            line_range{
                                                sub.lr_start + 1,
                                                sub.lr_end,
                                            });
            break;
        case '|':
            break;
        case '/':
            lnav::snippets::regex_highlighter(al,
                                              x,
                                              line_range{
                                                  sub.lr_start + 1,
                                                  sub.lr_end,
                                              });
            break;
    }
}

void
readline_lnav_highlighter(attr_line_t& al, std::optional<int> x)
{
    static const auto COMMENT_RE = lnav::pcre2pp::code::from_const(R"(^\s*#)");

    attr_line_builder alb(al);
    size_t start = 0, lf_pos;
    std::optional<size_t> section_start;

    while ((lf_pos = al.get_string().find('\n', start)) != std::string::npos) {
        line_range line{(int) start, (int) lf_pos};

        if (line.empty()) {
            start = lf_pos + 1;
            continue;
        }

        auto line_frag = string_fragment::from_str_range(
            al.al_string, line.lr_start, line.lr_end);

        auto find_res = COMMENT_RE.find_in(line_frag).ignore_error();
        if (find_res.has_value()) {
            if (section_start) {
                readline_lnav_highlighter_int(al,
                                              x,
                                              line_range{
                                                  (int) section_start.value(),
                                                  line.lr_start,
                                              });
                section_start = std::nullopt;
            }
            alb.overlay_attr(line_range{find_res->f_all.sf_begin, line.lr_end},
                             VC_ROLE.value(role_t::VCR_COMMENT));
        } else {
            switch (al.al_string[line.lr_start]) {
                case ':':
                case ';':
                case '|':
                case '/':
                    if (section_start) {
                        readline_lnav_highlighter_int(
                            al,
                            x,
                            line_range{
                                (int) section_start.value(),
                                line.lr_start,
                            });
                    }

                    section_start = line.lr_start;
                    break;
            }
        }

        start = lf_pos;
    }

    if (start == 0) {
        section_start = 0;
    }

    if (section_start) {
        readline_lnav_highlighter_int(al,
                                      x,
                                      line_range{
                                          (int) section_start.value(),
                                          (int) al.al_string.length(),
                                      });
    }
}

void
highlight_syntax(text_format_t tf, attr_line_t& al, std::optional<int> x)
{
    switch (tf) {
        case text_format_t::TF_SQL: {
            readline_sqlite_highlighter(al, x);
            break;
        }
        case text_format_t::TF_PCRE: {
            readline_regex_highlighter(al, x);
            break;
        }
        case text_format_t::TF_SHELL_SCRIPT: {
            readline_shlex_highlighter(al, x);
            break;
        }
        case text_format_t::TF_LNAV_SCRIPT: {
            readline_lnav_highlighter(al, x);
            break;
        }
        default:
            break;
    }
}
