# seams — WP3 + WP4 done

- **WP4** `77fb4216dd^` = `3e875a479b` "API: Add an in-process dispatch seam to KICAD_API_SERVER".
  `DispatchBytes()` = ready guard + parse + token check + Dispatch + serialize; `handleApiRequestString`
  is now `m_server->Reply( DispatchBytes( s ) )`. `StartInProcess( sink, reqUrl, eventsUrl )` runs with
  no KINNG server/publisher/wx queueing; `Publish` uses the sink (same sequence, same logging flag),
  and `Running/SocketPath/EventsSocketPath/Stop` honour `m_inProcess` (Stop publishes server_shutdown
  through the sink). `onApiRequest` keeps its own ready check, so the socket path is unchanged.
- **WP3** `77fb4216dd` "API: Extract API_SERVER_HOST…". All seven lifecycle hooks + captured locals
  moved verbatim into `include/api/api_server_host.h` + `common/api/api_server_host.cpp`
  (`Install/Preload/Shutdown`). `kicad/cli/command_api_server.cpp` 1043 → 150 lines.
- `PROJECT_TEMPLATE` moved to `include/project_template.h` + `common/project_template.cpp`.
  NOTE: those two file moves and the `common/CMakeLists.txt` COMMON_SRCS additions were swept into
  headless-build's commit `20475e3aa2` by a `git add -A` there; content is correct, just misattributed.
  `api_server_host.cpp` had to go in COMMON_SRCS, not KICOMMON_SRCS: API_HANDLER_COMMON and
  API_HANDLER_LIBRARY live in `common`, and kicommon (SHARED) cannot leave them undefined.
- Gate: `ninja -C build/dev kicad-cli pcbnew_kiface eeschema_kiface cvpcb_kiface` clean.
  `qa_api` is not a build/dev target (KICAD_BUILD_QA_TESTS=OFF); built in **build/qa**.
  `build/qa/qa/tests/api/qa_api --run_test=ApiInProcess` 6/6 pass; full qa_api 162 cases, no errors.
- fab_pcb conformance: **177 pass / 0 fail** before and after (identical; `bun test test/conformance`).
  There is no `test:integration` script in packages/client.
- **Environment fix, affects everyone:** Homebrew upgraded cmake 3.29→4.4.3, ninja→1.13.2 and
  protobuf 32→36, so `build/dev` could neither regenerate nor run (`libprotobuf.32.1.0.dylib` missing).
  I re-ran `cmake -S . -B build/dev` and `cmake -S . -B build/qa` and rebuilt both. Also built
  `cvpcb_kiface`, without which 6 ERC conformance checks fail.
  Pre-existing, not mine: `ninja -C build/dev kicad` fails in `kicad/project_tree.cpp`
  (`SetStateImages` — wx 3.3 API against wx 3.2); the 3 `TargetConditionals.h` errors are CMake's
  own compiler-flag probes and are harmless.
