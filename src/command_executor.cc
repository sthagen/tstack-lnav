/**
 * Copyright (c) 2015, Timothy Stack
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

#include <vector>

#include "command_executor.hh"

#include "base/ansi_scrubber.hh"
#include "base/ansi_vars.hh"
#include "base/fs_util.hh"
#include "base/humanize.hh"
#include "base/injector.hh"
#include "base/itertools.hh"
#include "base/paths.hh"
#include "base/string_util.hh"
#include "bound_tags.hh"
#include "config.h"
#include "curl_looper.hh"
#include "db_sub_source.hh"
#include "help_text_formatter.hh"
#include "lnav.hh"
#include "lnav.indexing.hh"
#include "lnav.prompt.hh"
#include "lnav_config.hh"
#include "lnav_util.hh"
#include "log_format_loader.hh"
#include "prql-modules.h"
#include "readline_highlighters.hh"
#include "service_tags.hh"
#include "shlex.hh"
#include "sql_help.hh"
#include "sql_util.hh"
#include "vtab_module.hh"

#ifdef HAVE_RUST_DEPS
#    include "prqlc.cxx.hh"
#endif

using namespace std::literals::chrono_literals;
using namespace lnav::roles::literals;

exec_context INIT_EXEC_CONTEXT;

static sig_atomic_t sql_counter = 0;

int
sql_progress(const log_cursor& lc)
{
    if (lnav_data.ld_window == nullptr) {
        return 0;
    }

    if (!lnav_data.ld_looping) {
        return 1;
    }

    if (ui_periodic_timer::singleton().time_to_update(sql_counter)) {
        ssize_t total = lnav_data.ld_log_source.text_line_count();
        off_t off = lc.lc_curr_line;

        if (off >= 0 && off <= total) {
            lnav_data.ld_bottom_source.update_loading(off, total);
            lnav_data.ld_status[LNS_BOTTOM].set_needs_update();
        }
        lnav_data.ld_status_refresher(lnav::func::op_type::blocking);
    }

    return 0;
}

void
sql_progress_finished()
{
    if (sql_counter == 0) {
        return;
    }

    sql_counter = 0;

    if (lnav_data.ld_window == nullptr) {
        return;
    }

    lnav_data.ld_bottom_source.update_loading(0, 0);
    lnav_data.ld_status[LNS_BOTTOM].set_needs_update();
    lnav_data.ld_status_refresher(lnav::func::op_type::blocking);
    lnav_data.ld_views[LNV_DB].redo_search();
}

static Result<std::string, lnav::console::user_message> execute_from_file(
    exec_context& ec,
    const std::string& src,
    int line_number,
    const std::string& cmdline);

Result<std::string, lnav::console::user_message>
execute_command(exec_context& ec, const std::string& cmdline)
{
    std::vector<std::string> args;

    log_info("Executing: %s", cmdline.c_str());

    split_ws(cmdline, args);

    if (!args.empty()) {
        readline_context::command_map_t::iterator iter;

        if ((iter = lnav_commands.find(args[0])) == lnav_commands.end()) {
            return ec.make_error("unknown command - {}", args[0]);
        }

        ec.ec_current_help = &iter->second->c_help;
        try {
            auto retval = iter->second->c_func(ec, cmdline, args);
            if (retval.isErr()) {
                auto um = retval.unwrapErr();

                ec.add_error_context(um);
                ec.ec_current_help = nullptr;
                return Err(um);
            }
            ec.ec_current_help = nullptr;

            return retval;
        } catch (const std::exception& e) {
            auto um = lnav::console::user_message::error(
                          attr_line_t("unexpected error while executing: ")
                              .append(lnav::roles::quoted_code(cmdline)))
                          .with_reason(e.what());
            return Err(um);
        }
    }

    return ec.make_error("no command to execute");
}

static Result<std::map<std::string, scoped_value_t>,
              lnav::console::user_message>
bind_sql_parameters(exec_context& ec, sqlite3_stmt* stmt)
{
    std::map<std::string, scoped_value_t> retval;
    auto param_count = sqlite3_bind_parameter_count(stmt);
    for (int lpc = 0; lpc < param_count; lpc++) {
        std::map<std::string, std::string>::iterator ov_iter;
        const auto* name = sqlite3_bind_parameter_name(stmt, lpc + 1);
        if (name == nullptr) {
            auto um
                = lnav::console::user_message::error("invalid SQL statement")
                      .with_reason(
                          "using a question-mark (?) for bound variables "
                          "is not supported, only named bound parameters "
                          "are supported")
                      .with_help(
                          "named parameters start with a dollar-sign "
                          "($) or colon (:) followed by the variable name")
                      .move();
            ec.add_error_context(um);

            return Err(um);
        }

        if (name[0] == '$') {
            const auto& lvars = ec.ec_local_vars.top();
            const auto& gvars = ec.ec_global_vars;
            std::map<std::string, scoped_value_t>::const_iterator local_var,
                global_var;
            const char* env_value;

            if (lnav_data.ld_window) {
                char buf[32];
                unsigned int lines, cols;

                ncplane_dim_yx(lnav_data.ld_window, &lines, &cols);
                if (strcmp(name, "$LINES") == 0) {
                    snprintf(buf, sizeof(buf), "%d", lines);
                    sqlite3_bind_text(stmt, lpc + 1, buf, -1, SQLITE_TRANSIENT);
                } else if (strcmp(name, "$COLS") == 0) {
                    snprintf(buf, sizeof(buf), "%d", cols);
                    sqlite3_bind_text(stmt, lpc + 1, buf, -1, SQLITE_TRANSIENT);
                }
            }

            if ((local_var = lvars.find(&name[1])) != lvars.end()) {
                mapbox::util::apply_visitor(
                    sqlitepp::bind_visitor(stmt, lpc + 1), local_var->second);
                retval[name] = local_var->second;
            } else if ((global_var = gvars.find(&name[1])) != gvars.end()) {
                mapbox::util::apply_visitor(
                    sqlitepp::bind_visitor(stmt, lpc + 1), global_var->second);
                retval[name] = global_var->second;
            } else if ((env_value = getenv(&name[1])) != nullptr) {
                sqlite3_bind_text(stmt, lpc + 1, env_value, -1, SQLITE_STATIC);
                retval[name] = string_fragment::from_c_str(env_value);
            }
        } else if (name[0] == ':' && ec.ec_line_values != nullptr) {
            for (auto& lv : ec.ec_line_values->lvv_values) {
                if (lv.lv_meta.lvm_name != &name[1]) {
                    continue;
                }
                switch (lv.lv_meta.lvm_kind) {
                    case value_kind_t::VALUE_BOOLEAN:
                        sqlite3_bind_int64(stmt, lpc + 1, lv.lv_value.i);
                        retval[name] = fmt::to_string(lv.lv_value.i);
                        break;
                    case value_kind_t::VALUE_FLOAT:
                        sqlite3_bind_double(stmt, lpc + 1, lv.lv_value.d);
                        retval[name] = fmt::to_string(lv.lv_value.d);
                        break;
                    case value_kind_t::VALUE_INTEGER:
                        sqlite3_bind_int64(stmt, lpc + 1, lv.lv_value.i);
                        retval[name] = fmt::to_string(lv.lv_value.i);
                        break;
                    case value_kind_t::VALUE_NULL:
                        sqlite3_bind_null(stmt, lpc + 1);
                        retval[name] = string_fragment::from_c_str(
                            db_label_source::NULL_STR);
                        break;
                    default:
                        sqlite3_bind_text(stmt,
                                          lpc + 1,
                                          lv.text_value(),
                                          lv.text_length(),
                                          SQLITE_TRANSIENT);
                        retval[name] = lv.to_string();
                        break;
                }
            }
        } else {
            sqlite3_bind_null(stmt, lpc + 1);
            log_warning("Could not bind variable: %s", name);
            retval[name]
                = string_fragment::from_c_str(db_label_source::NULL_STR);
        }
    }

    return Ok(retval);
}

static void
execute_search(const std::string& search_cmd)
{
    textview_curses* tc = get_textview_for_mode(lnav_data.ld_mode);
    auto search_term = string_fragment(search_cmd)
                           .find_right_boundary(0, string_fragment::tag1{'\n'})
                           .to_string();
    tc->execute_search(search_term);
}

Result<std::string, lnav::console::user_message>
execute_sql(exec_context& ec, const std::string& sql, std::string& alt_msg)
{
    auto_mem<sqlite3_stmt> stmt(sqlite3_finalize);
    timeval start_tv, end_tv;
    std::string stmt_str = trim(sql);
    std::string retval;
    int retcode = SQLITE_OK;

    if (lnav::sql::is_prql(stmt_str)) {
        log_info("compiling PRQL: %s", stmt_str.c_str());

#if HAVE_RUST_DEPS
        auto opts = prqlc::Options{true, "sql.sqlite", true};

        auto tree = sqlite_extension_prql;
        for (const auto& mod : lnav_prql_modules) {
            log_debug("prqlc adding mod %s", mod.get_name());
            tree.emplace_back(prqlc::SourceTreeElement{
                mod.get_name(),
                mod.to_string_fragment_producer()->to_string(),
            });
        }
        tree.emplace_back(prqlc::SourceTreeElement{"", stmt_str});
        log_debug("BEGIN compiling tree");
        auto cr = prqlc::compile_tree(tree, opts);
        log_debug("END compiling tree");

        for (const auto& msg : cr.messages) {
            if (msg.kind != prqlc::MessageKind::Error) {
                continue;
            }

            auto stmt_al = attr_line_t(stmt_str);
            readline_sqlite_highlighter(stmt_al, 0);
            auto um
                = lnav::console::user_message::error(
                      attr_line_t("unable to compile PRQL: ").append(stmt_al))
                      .with_reason(
                          attr_line_t::from_ansi_str((std::string) msg.reason));
            if (!msg.display.empty()) {
                um.with_note(
                    attr_line_t::from_ansi_str((std::string) msg.display));
            }
            for (const auto& hint : msg.hints) {
                um.with_help(attr_line_t::from_ansi_str((std::string) hint));
                break;
            }
            return Err(um);
        }
        log_debug("done!");
        stmt_str = (std::string) cr.output;
#else
        auto um = lnav::console::user_message::error(
            attr_line_t("PRQL is not supported in this build"));
        return Err(um);
#endif
    }

    log_info("Executing SQL: %s", stmt_str.c_str());

    auto old_mode = lnav_data.ld_mode;
    lnav_data.ld_mode = ln_mode_t::BUSY;
    auto mode_fin = finally([old_mode]() { lnav_data.ld_mode = old_mode; });
    lnav_data.ld_bottom_source.grep_error("");
    lnav_data.ld_status[LNS_BOTTOM].set_needs_update();

    if (startswith(stmt_str, ".")) {
        std::vector<std::string> args;
        split_ws(stmt_str, args);

        const auto* sql_cmd_map
            = injector::get<readline_context::command_map_t*,
                            sql_cmd_map_tag>();
        auto cmd_iter = sql_cmd_map->find(args[0]);

        if (cmd_iter != sql_cmd_map->end()) {
            ec.ec_current_help = &cmd_iter->second->c_help;
            auto retval = cmd_iter->second->c_func(ec, stmt_str, args);
            ec.ec_current_help = nullptr;

            return retval;
        }
    }

    ec.ec_accumulator->clear();

    require(!ec.ec_source.empty());
    const auto& source = ec.ec_source.back();
    sql_progress_guard progress_guard(sql_progress,
                                      sql_progress_finished,
                                      source.s_location,
                                      source.s_content);
    gettimeofday(&start_tv, nullptr);

    const auto* curr_stmt = stmt_str.c_str();
    auto last_is_readonly = false;
    while (curr_stmt != nullptr) {
        const char* tail = nullptr;
        while (isspace(*curr_stmt)) {
            curr_stmt += 1;
        }
        retcode = sqlite3_prepare_v2(
            lnav_data.ld_db.in(), curr_stmt, -1, stmt.out(), &tail);
        if (retcode != SQLITE_OK) {
            const char* errmsg = sqlite3_errmsg(lnav_data.ld_db);

            alt_msg = "";

            auto um = lnav::console::user_message::error(
                          "failed to compile SQL statement")
                          .with_reason(errmsg)
                          .with_snippets(ec.ec_source)
                          .move();

            auto annotated_sql = annotate_sql_with_error(
                lnav_data.ld_db.in(), curr_stmt, tail);
            auto loc = um.um_snippets.back().s_location;
            if (curr_stmt == stmt_str.c_str()) {
                um.um_snippets.pop_back();
            } else {
                auto tail_iter = stmt_str.begin();

                std::advance(tail_iter, (curr_stmt - stmt_str.c_str()));
                loc.sl_line_number
                    += std::count(stmt_str.begin(), tail_iter, '\n');
            }

            um.with_snippet(lnav::console::snippet::from(loc, annotated_sql));

            return Err(um);
        }
        if (stmt == nullptr) {
            retcode = SQLITE_DONE;
            break;
        }
#ifdef HAVE_SQLITE3_STMT_READONLY
        last_is_readonly = sqlite3_stmt_readonly(stmt.in());
        if (ec.is_read_only() && !last_is_readonly) {
            return ec.make_error(
                "modifying statements are not allowed in this context: {}",
                sql);
        }
#endif
        bool done = false;

        auto bound_values = TRY(bind_sql_parameters(ec, stmt.in()));
        if (last_is_readonly) {
            ec.ec_sql_callback(ec, stmt.in());
        }
        while (!done) {
            retcode = sqlite3_step(stmt.in());

            switch (retcode) {
                case SQLITE_OK:
                case SQLITE_DONE: {
                    auto changes = sqlite3_changes(lnav_data.ld_db.in());

                    log_info("sqlite3_changes() -> %d", changes);
                    done = true;
                    break;
                }

                case SQLITE_ROW:
                    ec.ec_sql_callback(ec, stmt.in());
                    break;

                default: {
                    attr_line_t bound_note;

                    if (!bound_values.empty()) {
                        bound_note.append(
                            "the bound parameters are set as follows:\n");
                        for (const auto& bval : bound_values) {
                            auto val_as_str = fmt::to_string(bval.second);
                            auto sql_type = bval.second.match(
                                [](const std::string&) { return SQLITE_TEXT; },
                                [](const string_fragment&) {
                                    return SQLITE_TEXT;
                                },
                                [](bool) { return SQLITE_INTEGER; },
                                [](int64_t) { return SQLITE_INTEGER; },
                                [](null_value_t) { return SQLITE_NULL; },
                                [](double) { return SQLITE_FLOAT; });
                            auto scrubbed_val = scrub_ws(val_as_str.c_str());
                            truncate_to(scrubbed_val, 40);
                            bound_note.append("  ")
                                .append(lnav::roles::variable(bval.first))
                                .append(":")
                                .append(sqlite3_type_to_string(sql_type))
                                .append(" = ")
                                .append_quoted(scrubbed_val)
                                .append("\n");
                        }
                    }

                    log_error("sqlite3_step error code: %d", retcode);
                    auto um = sqlite3_error_to_user_message(lnav_data.ld_db)
                                  .with_context_snippets(ec.ec_source)
                                  .remove_internal_snippets()
                                  .with_note(bound_note)
                                  .move();

                    return Err(um);
                }
            }
        }

        curr_stmt = tail;
    }

    if (last_is_readonly && !ec.ec_label_source_stack.empty()) {
        auto& dls = *ec.ec_label_source_stack.back();
        dls.dls_query_end = std::chrono::steady_clock::now();

        size_t memory_usage = 0, total_size = 0, cached_chunks = 0;
        for (auto cc = dls.dls_cell_container.cc_first.get(); cc != nullptr;
             cc = cc->cc_next.get())
        {
            total_size += cc->cc_capacity;
            if (cc->cc_data) {
                cached_chunks += 1;
                memory_usage += cc->cc_capacity;
            } else {
                memory_usage += cc->cc_compressed_size;
            }
        }
        log_debug(
            "cell memory footprint: total=%zu; actual=%zu; cached-chunks=%zu",
            total_size,
            memory_usage,
            cached_chunks);
    }
    gettimeofday(&end_tv, nullptr);
    lnav_data.ld_mode = old_mode;
    if (retcode == SQLITE_DONE) {
        if (lnav_data.ld_log_source.is_line_meta_changed()) {
            lnav_data.ld_log_source.text_filters_changed();
            lnav_data.ld_views[LNV_LOG].reload_data();
        }
        lnav_data.ld_filter_view.reload_data();
        lnav_data.ld_files_view.reload_data();

        lnav_data.ld_active_files.fc_files
            | lnav::itertools::for_each(&logfile::dump_stats);
        if (ec.ec_sql_callback != sql_callback) {
            retval = ec.ec_accumulator->get_string();
        } else {
            auto& dls = *(ec.ec_label_source_stack.back());
            if (!dls.dls_row_cursors.empty()) {
                lnav_data.ld_views[LNV_DB].reload_data();
                lnav_data.ld_views[LNV_DB].set_top(0_vl);
                lnav_data.ld_views[LNV_DB].set_left(0);
                if (lnav_data.ld_flags & LNF_HEADLESS) {
                    if (ec.ec_local_vars.size() == 1) {
                        ensure_view(&lnav_data.ld_views[LNV_DB]);
                    }

                    retval = "";
                    alt_msg = "";
                } else if (dls.dls_row_cursors.size() == 1) {
                    retval = dls.get_row_as_string(0_vl);
                } else {
                    int row_count = dls.dls_row_cursors.size();
                    char row_count_buf[128];
                    timeval diff_tv;

                    timersub(&end_tv, &start_tv, &diff_tv);
                    snprintf(row_count_buf,
                             sizeof(row_count_buf),
                             ANSI_BOLD("%'d") " row%s matched in " ANSI_BOLD(
                                 "%ld.%03ld") " seconds",
                             row_count,
                             row_count == 1 ? "" : "s",
                             diff_tv.tv_sec,
                             std::max((long) diff_tv.tv_usec / 1000, 1L));
                    retval = row_count_buf;
                    if (dls.has_log_time_column()) {
                        alt_msg
                            = HELP_MSG_1(Q,
                                         "to switch back to the previous view "
                                         "at the matching 'log_time' value");
                    } else {
                        alt_msg = "";
                    }
                }
            }
#ifdef HAVE_SQLITE3_STMT_READONLY
            else if (last_is_readonly)
            {
                retval = "info: No rows matched";
                alt_msg = "";

                if (lnav_data.ld_flags & LNF_HEADLESS) {
                    if (ec.ec_local_vars.size() == 1) {
                        lnav_data.ld_views[LNV_DB].reload_data();
                        ensure_view(&lnav_data.ld_views[LNV_DB]);
                    }
                }
            }
#endif
        }
    }

    return Ok(retval);
}

Result<void, lnav::console::user_message>
multiline_executor::handle_command(const std::string& cmdline)
{
    this->me_last_result = TRY(execute_from_file(this->me_exec_context,
                                                 this->p_source,
                                                 this->p_starting_line_number,
                                                 cmdline));

    return Ok();
}

static Result<std::string, lnav::console::user_message>
execute_file_contents(exec_context& ec, const std::filesystem::path& path)
{
    static const std::filesystem::path stdin_path("-");
    static const std::filesystem::path dev_stdin_path("/dev/stdin");

    FILE* file;

    if (path == stdin_path || path == dev_stdin_path) {
        if (isatty(STDIN_FILENO)) {
            return ec.make_error("stdin has already been consumed");
        }
        file = stdin;
    } else if ((file = fopen(path.c_str(), "re")) == nullptr) {
        return ec.make_error("unable to open file");
    }

    std::string out_name;

    auto_mem<char> line;
    size_t line_max_size;
    ssize_t line_size;
    multiline_executor me(ec, path.string());

    ec.ec_local_vars.top()["0"] = path.string();
    ec.ec_path_stack.emplace_back(path.parent_path());
    exec_context::output_guard og(ec);
    while ((line_size = getline(line.out(), &line_max_size, file)) != -1) {
        TRY(me.push_back(string_fragment::from_bytes(line.in(), line_size)));
    }

    TRY(me.final());
    auto retval = std::move(me.me_last_result);

    if (file == stdin) {
        if (isatty(STDOUT_FILENO)) {
            log_perror(dup2(STDOUT_FILENO, STDIN_FILENO));
        }
    } else {
        fclose(file);
    }
    ec.ec_path_stack.pop_back();

    return Ok(retval);
}

Result<std::string, lnav::console::user_message>
execute_file(exec_context& ec, const std::string& path_and_args)
{
    static const intern_string_t SRC = intern_string::lookup("cmdline");

    std::string retval, msg;
    shlex lexer(path_and_args);

    log_info("Executing file: %s", path_and_args.c_str());

    auto split_args_res = lexer.split(scoped_resolver{&ec.ec_local_vars.top()});
    if (split_args_res.isErr()) {
        auto split_err = split_args_res.unwrapErr();
        auto um = lnav::console::user_message::error(
                      "unable to parse script command-line")
                      .with_reason(split_err.se_error.te_msg)
                      .with_snippet(lnav::console::snippet::from(
                          SRC, lexer.to_attr_line(split_err.se_error)))
                      .move();

        return Err(um);
    }
    auto split_args = split_args_res.unwrap();
    if (split_args.empty()) {
        return ec.make_error("no script specified");
    }

    ec.ec_local_vars.emplace();

    auto script_name = split_args[0].se_value;
    auto& vars = ec.ec_local_vars.top();
    std::string star, open_error = "file not found";

    add_ansi_vars(vars);

    vars["#"] = fmt::to_string(split_args.size() - 1);
    for (size_t lpc = 0; lpc < split_args.size(); lpc++) {
        vars[fmt::to_string(lpc)] = split_args[lpc].se_value;
    }
    for (size_t lpc = 1; lpc < split_args.size(); lpc++) {
        if (lpc > 1) {
            star.append(" ");
        }
        star.append(split_args[lpc].se_value);
    }
    vars["__all__"] = star;

    std::vector<script_metadata> paths_to_exec;

    auto scripts = find_format_scripts(lnav_data.ld_config_paths);
    auto iter = scripts.as_scripts.find(script_name);
    if (iter != scripts.as_scripts.end()) {
        paths_to_exec = iter->second;
    }
    if (is_url(script_name)) {
        auto_mem<CURLU> cu(curl_url_cleanup);
        cu = curl_url();
        auto set_rc = curl_url_set(cu, CURLUPART_URL, script_name.c_str(), 0);
        if (set_rc == CURLUE_OK) {
            auto_mem<char> scheme_part(curl_free);
            auto get_rc
                = curl_url_get(cu, CURLUPART_SCHEME, scheme_part.out(), 0);
            if (get_rc == CURLUE_OK
                && string_fragment::from_c_str(scheme_part.in()) == "file")
            {
                auto_mem<char> path_part;
                auto get_rc
                    = curl_url_get(cu, CURLUPART_PATH, path_part.out(), 0);
                if (get_rc == CURLUE_OK) {
                    auto rp_res = lnav::filesystem::realpath(path_part.in());
                    if (rp_res.isOk()) {
                        struct script_metadata meta;

                        meta.sm_path = rp_res.unwrap();
                        extract_metadata_from_file(meta);
                        paths_to_exec.push_back(meta);
                    }
                }
            }
        }
    }
    if (script_name == "-" || script_name == "/dev/stdin") {
        paths_to_exec.push_back({script_name, "", "", ""});
    } else if (access(script_name.c_str(), R_OK) == 0) {
        struct script_metadata meta;
        auto rp_res = lnav::filesystem::realpath(script_name);

        if (rp_res.isErr()) {
            log_error("unable to get realpath() of %s -- %s",
                      script_name.c_str(),
                      rp_res.unwrapErr().c_str());
            meta.sm_path = script_name;
        } else {
            meta.sm_path = rp_res.unwrap();
        }
        extract_metadata_from_file(meta);
        paths_to_exec.push_back(meta);
    } else if (errno != ENOENT) {
        open_error = strerror(errno);
    } else {
        auto script_path = std::filesystem::path(script_name);

        if (!script_path.is_absolute()) {
            script_path = ec.ec_path_stack.back() / script_path;
        }

        if (std::filesystem::is_regular_file(script_path)) {
            script_metadata meta;

            meta.sm_path = script_path;
            extract_metadata_from_file(meta);
            paths_to_exec.push_back(meta);
        } else if (errno != ENOENT) {
            open_error = strerror(errno);
        }
    }

    if (!paths_to_exec.empty()) {
        for (auto& path_iter : paths_to_exec) {
            if (!ec.ec_output_stack.empty()) {
                ec.ec_output_stack.back().od_format
                    = path_iter.sm_output_format;
                log_info("%s: setting out format for '%s' to %d",
                         script_name.c_str(),
                         ec.ec_output_stack.back().od_name.c_str(),
                         ec.ec_output_stack.back().od_format);
            }
            retval = TRY(execute_file_contents(ec, path_iter.sm_path));
        }
    }
    ec.ec_local_vars.pop();

    if (paths_to_exec.empty()) {
        return ec.make_error(
            "unknown script -- {} -- {}", script_name, open_error);
    }

    return Ok(retval);
}

Result<std::string, lnav::console::user_message>
execute_from_file(exec_context& ec,
                  const std::string& src,
                  int line_number,
                  const std::string& cmdline)
{
    std::string retval, alt_msg;
    auto _sg
        = ec.enter_source(intern_string::lookup(src), line_number, cmdline);

    lnav_data.ld_view_stack.top() | [&ec](auto* tc) {
        ec.ec_top_line = tc->get_selection().value_or(0_vl);
    };
    switch (cmdline[0]) {
        case ':':
            retval = TRY(execute_command(ec, cmdline.substr(1)));
            break;
        case '/':
            execute_search(cmdline.substr(1));
            break;
        case ';': {
            if (!ec.ec_msg_callback_stack.empty()) {
                auto src_slash = src.rfind('/');
                auto cmd_al
                    = attr_line_t(cmdline.substr(0, cmdline.find('\n')));
                readline_lnav_highlighter(cmd_al, std::nullopt);
                const auto um = lnav::console::user_message::info(
                                    attr_line_t("Executing command at ")
                                        .append(lnav::roles::file(
                                            src_slash != std::string::npos
                                                ? src.substr(src_slash + 1)
                                                : src))
                                        .append(":")
                                        .append(lnav::roles::number(
                                            fmt::to_string(line_number)))
                                        .append(" \u2014 ")
                                        .append(cmd_al))
                                    .move();
                ec.ec_msg_callback_stack.back()(um);
            }

            setup_logline_table(ec);
            retval = TRY(execute_sql(ec, cmdline.substr(1), alt_msg));
            break;
        }
        case '|':
            retval = TRY(execute_file(ec, cmdline.substr(1)));
            break;
        default:
            retval = TRY(execute_command(ec, cmdline));
            break;
    }

    log_info(
        "%s:%d:execute result -- %s", src.c_str(), line_number, retval.c_str());

    return Ok(retval);
}

Result<std::string, lnav::console::user_message>
execute_any(exec_context& ec, const std::string& cmdline_with_mode)
{
    if (cmdline_with_mode.empty()) {
        auto um = lnav::console::user_message::error("empty command")
                      .with_help(
                          "a command should start with ':', ';', '/', '|' and "
                          "followed by the operation to perform")
                      .move();
        if (!ec.ec_source.empty()) {
            um.with_snippet(ec.ec_source.back());
        }
        return Err(um);
    }

    std::string retval, alt_msg, cmdline = cmdline_with_mode.substr(1);
    auto _cleanup = finally([&ec] {
        if (ec.is_read_write() &&
            // only rebuild in a script or non-interactive mode so we don't
            // block the UI.
            lnav_data.ld_flags & LNF_HEADLESS)
        {
            rescan_files();
            wait_for_pipers(std::nullopt);
            rebuild_indexes_repeatedly();
        }
    });

    switch (cmdline_with_mode[0]) {
        case ':':
            retval = TRY(execute_command(ec, cmdline));
            break;
        case '/':
            execute_search(cmdline);
            break;
        case ';':
            setup_logline_table(ec);
            retval = TRY(execute_sql(ec, cmdline, alt_msg));
            break;
        case '|': {
            retval = TRY(execute_file(ec, cmdline));
            break;
        }
        default:
            retval = TRY(execute_command(ec, cmdline));
            break;
    }

    return Ok(retval);
}

void
execute_init_commands(
    exec_context& ec,
    std::vector<std::pair<Result<std::string, lnav::console::user_message>,
                          std::string>>& msgs)
{
    if (lnav_data.ld_cmd_init_done) {
        return;
    }

    std::string out_name;
    std::optional<exec_context::output_t> ec_out;
    auto_fd fd_copy;
    auto& dls = *(ec.ec_label_source_stack.back());
    int option_index = 1;

    {
        log_info("Executing initial commands");
        exec_context::output_guard og(ec, out_name, ec_out);

        for (auto& cmd : lnav_data.ld_commands) {
            static const auto COMMAND_OPTION_SRC
                = intern_string::lookup("command-option");

            std::string alt_msg;
            auto has_db_query = false;

            wait_for_children();

            lnav_data.ld_view_stack.top() | [&ec](auto* tc) {
                ec.ec_top_line = tc->get_selection().value_or(0_vl);
            };
            log_debug("init cmd: %s", cmd.c_str());
            {
                auto _sg
                    = ec.enter_source(COMMAND_OPTION_SRC, option_index++, cmd);
                switch (cmd.at(0)) {
                    case ':':
                        msgs.emplace_back(execute_command(ec, cmd.substr(1)),
                                          alt_msg);
                        break;
                    case '/':
                        execute_search(cmd.substr(1));
                        break;
                    case ';':
                        setup_logline_table(ec);
                        msgs.emplace_back(
                            execute_sql(ec, cmd.substr(1), alt_msg), alt_msg);
                        has_db_query = true;
                        break;
                    case '|':
                        msgs.emplace_back(execute_file(ec, cmd.substr(1)),
                                          alt_msg);
                        break;
                }

                rescan_files();
                auto deadline = ui_clock::now();
                if (lnav_data.ld_flags & LNF_HEADLESS) {
                    deadline += 5s;
                } else {
                    deadline += 500ms;
                }
                wait_for_pipers(deadline);
                rebuild_indexes_repeatedly();
            }
            if (has_db_query && !dls.dls_headers.empty()
                && lnav_data.ld_view_stack.size() == 1)
            {
                lnav_data.ld_views[LNV_DB].reload_data();
                ensure_view(LNV_DB);
            }
        }
    }
    lnav_data.ld_commands.clear();
    lnav_data.ld_cmd_init_done = true;
}

int
sql_callback(exec_context& ec, sqlite3_stmt* stmt)
{
    auto& dls = *(ec.ec_label_source_stack.back());
    const int ncols = sqlite3_column_count(stmt);

    if (!sqlite3_stmt_busy(stmt)) {
        dls.clear();

        for (int lpc = 0; lpc < ncols; lpc++) {
            const int type = sqlite3_column_type(stmt, lpc);
            std::string colname = sqlite3_column_name(stmt, lpc);

            dls.push_header(colname, type);
        }
        return 0;
    }

    int retval = 0;
    auto set_vars = dls.dls_row_cursors.empty();

    if (dls.dls_row_cursors.empty()) {
        dls.dls_query_start = std::chrono::steady_clock::now();
        for (int lpc = 0; lpc < ncols; lpc++) {
            int type = sqlite3_column_type(stmt, lpc);
            std::string colname = sqlite3_column_name(stmt, lpc);

            bool graphable = (type == SQLITE_INTEGER || type == SQLITE_FLOAT);
            auto& hm = dls.dls_headers[lpc];
            hm.hm_column_type = type;
            if (lnav_data.ld_db_key_names.count(colname) > 0) {
                hm.hm_graphable = false;
            } else if (graphable) {
                dls.set_col_as_graphable(lpc);
            }
        }
    }

    dls.dls_row_cursors.emplace_back(dls.dls_cell_container.end_cursor());
    dls.dls_push_column = 0;
    for (int lpc = 0; lpc < ncols; lpc++) {
        const auto value_type = sqlite3_column_type(stmt, lpc);
        db_label_source::column_value_t value;
        auto& hm = dls.dls_headers[lpc];

        switch (value_type) {
            case SQLITE_INTEGER:
                value = (int64_t) sqlite3_column_int64(stmt, lpc);
                hm.hm_align = text_align_t::end;
                break;
            case SQLITE_FLOAT:
                value = sqlite3_column_double(stmt, lpc);
                hm.hm_align = text_align_t::end;
                break;
            case SQLITE_NULL:
                value = null_value_t{};
                break;
            default: {
                auto frag = string_fragment::from_bytes(
                    sqlite3_column_text(stmt, lpc),
                    sqlite3_column_bytes(stmt, lpc));
                if (!frag.empty()) {
                    if (isdigit(frag[0])) {
                        hm.hm_align = text_align_t::end;
                        if (!hm.hm_graphable.has_value()) {
                            auto split_res = humanize::try_from<double>(frag);
                            if (split_res.has_value()) {
                                dls.set_col_as_graphable(lpc);
                            } else {
                                hm.hm_graphable = false;
                            }
                        }
                    } else if (!hm.hm_graphable.has_value()) {
                        hm.hm_graphable = false;
                    }
                }
                value = frag;
                break;
            }
        }
        dls.push_column(value);
        if ((hm.hm_column_type == SQLITE_TEXT
             || hm.hm_column_type == SQLITE_NULL)
            && hm.hm_sub_type == 0)
        {
            switch (value_type) {
                case SQLITE_TEXT:
                    auto* raw_value = sqlite3_column_value(stmt, lpc);
                    hm.hm_column_type = SQLITE_TEXT;
                    hm.hm_sub_type = sqlite3_value_subtype(raw_value);
                    break;
            }
        }
        if (set_vars && !ec.ec_local_vars.empty() && !ec.ec_dry_run) {
            if (sql_ident_needs_quote(hm.hm_name.c_str())) {
                continue;
            }
            auto& vars = ec.ec_local_vars.top();

            vars[hm.hm_name] = value.match(
                [](const string_fragment& sf) {
                    return scoped_value_t{sf.to_string()};
                },
                [](int64_t i) { return scoped_value_t{i}; },
                [](double d) { return scoped_value_t{d}; },
                [](null_value_t) { return scoped_value_t{null_value_t{}}; });
        }
    }

    return retval;
}

int
internal_sql_callback(exec_context& ec, sqlite3_stmt* stmt)
{
    if (!sqlite3_stmt_busy(stmt)) {
        return 0;
    }

    const int ncols = sqlite3_column_count(stmt);

    auto& vars = ec.ec_local_vars.top();

    for (int lpc = 0; lpc < ncols; lpc++) {
        const char* column_name = sqlite3_column_name(stmt, lpc);

        if (sql_ident_needs_quote(column_name)) {
            continue;
        }

        auto value_type = sqlite3_column_type(stmt, lpc);
        scoped_value_t value;
        switch (value_type) {
            case SQLITE_INTEGER:
                value = (int64_t) sqlite3_column_int64(stmt, lpc);
                break;
            case SQLITE_FLOAT:
                value = sqlite3_column_double(stmt, lpc);
                break;
            case SQLITE_NULL:
                value = null_value_t{};
                break;
            default:
                value
                    = std::string((const char*) sqlite3_column_text(stmt, lpc),
                                  sqlite3_column_bytes(stmt, lpc));
                break;
        }
        vars[column_name] = value;
    }

    return 0;
}

std::future<std::string>
pipe_callback(exec_context& ec, const std::string& cmdline, auto_fd& fd)
{
    static auto& prompt = lnav::prompt::get();
    auto out = ec.get_output();

    if (out) {
        FILE* file = *out;

        return std::async(std::launch::async, [fd = std::move(fd), file]() {
            char buffer[1024];
            ssize_t rc;

            if (file == stdout) {
                lnav_data.ld_stdout_used = true;
            }

            while ((rc = read(fd, buffer, sizeof(buffer))) > 0) {
                fwrite(buffer, rc, 1, file);
            }

            return std::string();
        });
    }
    std::error_code errc;
    std::filesystem::create_directories(lnav::paths::workdir(), errc);
    auto open_temp_res = lnav::filesystem::open_temp_file(lnav::paths::workdir()
                                                          / "exec.XXXXXX");
    if (open_temp_res.isErr()) {
        return lnav::futures::make_ready_future(
            fmt::format(FMT_STRING("error: cannot open temp file -- {}"),
                        open_temp_res.unwrapErr()));
    }

    auto tmp_pair = open_temp_res.unwrap();

    auto reader = std::thread(
        [in_fd = std::move(fd), out_fd = std::move(tmp_pair.second)]() {
            char buffer[1024];
            ssize_t rc;

            while ((rc = read(in_fd, buffer, sizeof(buffer))) > 0) {
                write(out_fd, buffer, rc);
            }
        });
    reader.detach();

    static int exec_count = 0;
    auto desc
        = fmt::format(FMT_STRING("exec-{}-output {}"), exec_count++, cmdline);
    lnav_data.ld_active_files.fc_file_names[tmp_pair.first]
        .with_filename(desc)
        .with_include_in_session(false)
        .with_detect_format(false)
        .with_init_location(0_vl);
    lnav_data.ld_files_to_front.emplace_back(desc);
    prompt.p_editor.set_alt_value(HELP_MSG_1(X, "to close the file"));

    return lnav::futures::make_ready_future(std::string());
}

void
add_global_vars(exec_context& ec)
{
    for (const auto& iter : lnav_config.lc_global_vars) {
        shlex subber(iter.second);
        std::string str;

        if (!subber.eval(str, scoped_resolver{&ec.ec_global_vars})) {
            log_error("Unable to evaluate global variable value: %s",
                      iter.second.c_str());
            continue;
        }

        ec.ec_global_vars[iter.first] = str;
    }
}

void
exec_context::set_output(const std::string& name,
                         FILE* file,
                         int (*closer)(FILE*))
{
    log_info("redirecting command output to: %s", name.c_str());
    this->ec_output_stack.back().od_output | [](auto out) {
        if (out.second != nullptr) {
            out.second(out.first);
        }
    };
    this->ec_output_stack.back()
        = output_desc{name, std::make_pair(file, closer)};
}

void
exec_context::clear_output()
{
    log_info("redirecting command output to screen");
    this->ec_output_stack.back().od_output | [](auto out) {
        if (out.second != nullptr) {
            out.second(out.first);
        }
    };
    this->ec_output_stack.back().od_name = "default";
    this->ec_output_stack.back().od_output = std::nullopt;
}

exec_context::exec_context(logline_value_vector* line_values,
                           sql_callback_t sql_callback,
                           pipe_callback_t pipe_callback)
    : ec_line_values(line_values),
      ec_accumulator(std::make_unique<attr_line_t>()),
      ec_sql_callback(sql_callback), ec_pipe_callback(pipe_callback)
{
    this->ec_local_vars.push(std::map<std::string, scoped_value_t>());
    this->ec_path_stack.emplace_back(".");
    this->ec_output_stack.emplace_back("screen", std::nullopt);
}

Result<std::string, lnav::console::user_message>
exec_context::execute(source_location loc, const std::string& cmdline)
{
    static auto& prompt = lnav::prompt::get();
    static const auto& dls = lnav_data.ld_db_row_source;

    lnav::textinput::history::op_guard hist_guard;
    auto sg = this->enter_source(loc, cmdline);

    auto before_dls_gen = dls.dls_generation;
    if (this->get_provenance<mouse_input>() && !prompt.p_editor.is_enabled()) {
        auto& hist = prompt.get_history_for(cmdline[0]);
        hist_guard = hist.start_operation(cmdline.substr(1));
    }

    auto exec_res = execute_any(*this, cmdline);
    if (exec_res.isErr()) {
        hist_guard.og_status = log_level_t::LEVEL_ERROR;
        if (!this->ec_msg_callback_stack.empty()) {
            this->ec_msg_callback_stack.back()(exec_res.unwrapErr());
        }
    } else {
        if (before_dls_gen != dls.dls_generation
            && dls.dls_row_cursors.size() > 1)
        {
            ensure_view(LNV_DB);
        }
    }

    return exec_res;
}

void
exec_context::add_error_context(lnav::console::user_message& um)
{
    switch (um.um_level) {
        case lnav::console::user_message::level::raw:
        case lnav::console::user_message::level::info:
        case lnav::console::user_message::level::ok:
            return;
        default:
            break;
    }

    if (um.um_snippets.empty()) {
        um.with_snippets(this->ec_source);
        um.remove_internal_snippets();
    }

    if (this->ec_current_help != nullptr && um.um_help.empty()) {
        attr_line_t help;

        format_help_text_for_term(*this->ec_current_help,
                                  70,
                                  help,
                                  help_text_content::synopsis_and_summary);
        um.with_help(help);
    }
}

exec_context::sql_callback_guard
exec_context::push_callback(sql_callback_t cb)
{
    return sql_callback_guard(*this, cb);
}

exec_context::source_guard
exec_context::enter_source(source_location loc, const std::string& content)
{
    attr_line_t content_al{content};
    content_al.with_attr_for_all(VC_ROLE.value(role_t::VCR_QUOTED_CODE));
    readline_lnav_highlighter(content_al, -1);
    this->ec_source.emplace_back(lnav::console::snippet::from(loc, content_al));
    return {this};
}

exec_context::output_guard::output_guard(exec_context& context,
                                         std::string name,
                                         const std::optional<output_t>& file,
                                         text_format_t tf)
    : sg_context(context), sg_active(!name.empty())
{
    if (name.empty()) {
        return;
    }
    if (file) {
        log_info("redirecting command output to: %s", name.c_str());
    } else if (!context.ec_output_stack.empty()) {
        tf = context.ec_output_stack.back().od_format;
    }
    context.ec_output_stack.emplace_back(std::move(name), file, tf);
}

exec_context::output_guard::~output_guard()
{
    if (this->sg_active) {
        this->sg_context.clear_output();
        this->sg_context.ec_output_stack.pop_back();
    }
}
exec_context::sql_callback_guard::sql_callback_guard(exec_context& context,
                                                     sql_callback_t cb)
    : scg_context(context), scg_old_callback(context.ec_sql_callback)
{
    context.ec_sql_callback = cb;
}
exec_context::sql_callback_guard::sql_callback_guard(sql_callback_guard&& other)
    : scg_context(other.scg_context),
      scg_old_callback(std::exchange(other.scg_old_callback, nullptr))
{
}
exec_context::sql_callback_guard::~sql_callback_guard()
{
    if (this->scg_old_callback != nullptr) {
        this->scg_context.ec_sql_callback = this->scg_old_callback;
    }
}
