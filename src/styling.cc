/**
 * Copyright (c) 2019, Timothy Stack
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
 */

#include <string>

#include "styling.hh"

#include "ansi-palette-json.h"
#include "base/from_trait.hh"
#include "config.h"
#include "css-color-names-json.h"
#include "fmt/format.h"
#include "xterm-palette-json.h"
#include "yajlpp/yajlpp.hh"
#include "yajlpp/yajlpp_def.hh"

static const json_path_container term_color_rgb_handler = {
    yajlpp::property_handler("r").for_field(&rgb_color::rc_r),
    yajlpp::property_handler("g").for_field(&rgb_color::rc_g),
    yajlpp::property_handler("b").for_field(&rgb_color::rc_b),
};

static const json_path_container term_color_handler = {
    yajlpp::property_handler("colorId").for_field(&term_color::xc_id),
    yajlpp::property_handler("name").for_field(&term_color::xc_name),
    yajlpp::property_handler("hexString").for_field(&term_color::xc_hex),
    yajlpp::property_handler("rgb")
        .for_child(&term_color::xc_color)
        .with_children(term_color_rgb_handler),
};

static const json_path_handler_base::enum_value_t _align_values[] = {
    {"start", text_align_t::start},
    {"center", text_align_t::center},
    {"end", text_align_t::end},

    json_path_handler_base::ENUM_TERMINATOR,
};

const json_path_container style_config_handlers =
    json_path_container{
        yajlpp::property_handler("text-align")
        .with_synopsis("start|center|end")
        .with_enum_values(_align_values)
        .with_description("How to align text within a cell")
        .for_field(&style_config::sc_text_align),
        yajlpp::property_handler("color")
            .with_synopsis("#hex|color_name")
            .with_description(
                "The foreground color value for this style. The value can be "
                "the name of an xterm color, the hexadecimal value, or a theme "
                "variable reference.")
            .with_example("#fff")
            .with_example("Green")
            .with_example("$black")
            .for_field(&style_config::sc_color),
        yajlpp::property_handler("background-color")
            .with_synopsis("#hex|color_name")
            .with_description(
                "The background color value for this style. The value can be "
                "the name of an xterm color, the hexadecimal value, or a theme "
                "variable reference.")
            .with_example("#2d2a2e")
            .with_example("Green")
            .for_field(&style_config::sc_background_color),
        yajlpp::property_handler("underline")
            .with_description("Indicates that the text should be underlined.")
            .for_field(&style_config::sc_underline),
        yajlpp::property_handler("bold")
            .with_description("Indicates that the text should be bolded.")
            .for_field(&style_config::sc_bold),
        yajlpp::property_handler("italic")
            .with_description("Indicates that the text should be italicized.")
            .for_field(&style_config::sc_italic),
        yajlpp::property_handler("strike")
            .with_description("Indicates that the text should be struck.")
            .for_field(&style_config::sc_strike),
    }
.with_definition_id("style");

static const typed_json_path_container<std::vector<term_color>>
    root_color_handler = {
        yajlpp::property_handler("#")
            .with_obj_provider<term_color, std::vector<term_color>>(
                [](const yajlpp_provider_context& ypc,
                   std::vector<term_color>* palette) {
                    if (ypc.ypc_index >= palette->size()) {
                        palette->resize(ypc.ypc_index + 1);
                    }
                    return &((*palette)[ypc.ypc_index]);
                })
            .with_children(term_color_handler),
};

struct css_color_names {
    std::map<std::string, std::string> ccn_name_to_color;
};

static const typed_json_path_container<css_color_names> css_color_names_handlers
    = {
        yajlpp::pattern_property_handler("(?<css_color_name>.*)")
            .for_field(&css_color_names::ccn_name_to_color),
};

static const css_color_names&
get_css_color_names()
{
    static const intern_string_t iname
        = intern_string::lookup(css_color_names_json.get_name());
    static auto sfp = css_color_names_json.to_string_fragment_producer();
    static const auto INSTANCE
        = css_color_names_handlers.parser_for(iname).of(*sfp).unwrap();

    return INSTANCE;
}

term_color_palette*
xterm_colors()
{
    static term_color_palette retval(
        xterm_palette_json.get_name(),
        *xterm_palette_json.to_string_fragment_producer());

    return &retval;
}

term_color_palette*
ansi_colors()
{
    static term_color_palette retval(
        ansi_palette_json.get_name(),
        *ansi_palette_json.to_string_fragment_producer());

    return &retval;
}

template<>
Result<rgb_color, std::string>
from(string_fragment sf)
{
    if (sf.empty()) {
        return Ok(rgb_color());
    }

    if (sf[0] != '#') {
        const auto& css_colors = get_css_color_names();
        const auto& iter = css_colors.ccn_name_to_color.find(sf.to_string());

        if (iter != css_colors.ccn_name_to_color.end()) {
            sf = string_fragment::from_str(iter->second);
        }
    }

    rgb_color rgb_out;

    if (sf[0] == '#') {
        switch (sf.length()) {
            case 4:
                if (sscanf(sf.data(),
                           "#%1hx%1hx%1hx",
                           &rgb_out.rc_r,
                           &rgb_out.rc_g,
                           &rgb_out.rc_b)
                    == 3)
                {
                    rgb_out.rc_r |= rgb_out.rc_r << 4;
                    rgb_out.rc_g |= rgb_out.rc_g << 4;
                    rgb_out.rc_b |= rgb_out.rc_b << 4;
                    return Ok(rgb_out);
                }
                break;
            case 7:
                if (sscanf(sf.data(),
                           "#%2hx%2hx%2hx",
                           &rgb_out.rc_r,
                           &rgb_out.rc_g,
                           &rgb_out.rc_b)
                    == 3)
                {
                    return Ok(rgb_out);
                }
                break;
        }

        return Err(fmt::format(FMT_STRING("Could not parse color: {}"), sf));
    }

    return Err(fmt::format(
        FMT_STRING(
            "Unknown color: '{}'.  "
            "See https://jonasjacek.github.io/colors/ for a list of supported "
            "color names"),
        sf));
}

term_color_palette::term_color_palette(const char* name,
                                       string_fragment_producer& json)
{
    intern_string_t iname = intern_string::lookup(name);
    auto parse_res
        = root_color_handler.parser_for(iname).with_ignore_unused(true).of(
            json);

    if (parse_res.isErr()) {
        log_error("failed to parse palette: %s -- %s",
                  name,
                  parse_res.unwrapErr()[0].to_attr_line().get_string().c_str());
    }
    require(parse_res.isOk());

    this->tc_palette = parse_res.unwrap();
    for (auto& xc : this->tc_palette) {
        xc.xc_lab_color = lab_color(xc.xc_color);
    }
}

uint8_t
term_color_palette::match_color(const lab_color& to_match) const
{
    double lowest = 1000.0;
    short lowest_id = -1;

    for (const auto& xc : this->tc_palette) {
        const double xc_delta = xc.xc_lab_color.deltaE(to_match);

        if (lowest_id == -1) {
            lowest = xc_delta;
            lowest_id = xc.xc_id;
            continue;
        }

        if (xc_delta < lowest) {
            lowest = xc_delta;
            lowest_id = xc.xc_id;
        }
    }

    require_ge(lowest_id, -100);

    return lowest_id;
}

namespace styling {

Result<color_unit, std::string>
color_unit::from_str(const string_fragment& sf)
{
    if (sf == "semantic()") {
        return Ok(color_unit{semantic{}});
    }

    if (!sf.startswith("#")) {
        for (const auto& xc : xterm_colors()->tc_palette) {
            if (sf.iequal(xc.xc_name)) {
                return Ok(from_palette(xc.xc_id));
            }
        }
    }
    auto retval = TRY(from<rgb_color>(sf));
    if (retval.empty()) {
        return Ok(EMPTY);
    }

    return Ok(color_unit{retval});
}

}  // namespace styling
