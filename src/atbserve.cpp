/*
  atbserve, an atomic syzygy tablebase server
  Copyright (C) 2016 Niklas Fiekas <niklas.fiekas@backscattering.de>

  based on

  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
#include <string>
#include <algorithm>

#include <getopt.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>

#include "bitboard.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"

namespace PSQT {
  void init();
}

namespace {

static int verbose = 0;  // --verbose
static int cors = 0;  // --cors

bool validate_fen(const char *fen) {
  // 1. Board setup
  for (int rank = 7; rank >= 0; rank--) {
      bool last_was_number = false;
      int file = 0;

      for (; file <= 7; file++) {
          char c = *fen++;

          if (c >= '1' && c <= '8') {
              if (last_was_number) return false;
              file += c - '1';
              last_was_number = true;
              continue;
          } else {
              last_was_number = false;
          }

          switch (c) {
              case 'k': case 'K':
              case 'p': case 'P':
              case 'n': case 'N':
              case 'b': case 'B':
              case 'r': case 'R':
              case 'q': case 'Q':
                  break;

              default:
                  return false;
          }
      }

      if (file != 8) return false;

      char c = *fen++;
      if (!c) return false;

      if (rank > 0) {
          if (c != '/') return false;
      } else {
          if (c != ' ') return false;
      }
  }

  // 2. Turn.
  char c = *fen++;
  if (c != 'w' && c != 'b') return false;
  if (*fen++ != ' ') return false;

  // 3. Castling
  c = *fen++;
  if (c != '-') {
      do {
          if (c >= 'a' && c <= 'h') continue;
          else if (c >= 'A' && c <= 'H') continue;
          else if (c == 'q' || c == 'Q') continue;
          else if (c == 'k' || c == 'K') continue;
          else return false;
      } while ((c = *fen++) != ' ');
  } else if (*fen++ != ' ') return false;

  // 4. En-passant
  c = *fen++;
  if (c != '-') {
      if (c < 'a' || c > 'h') return false;

      c = *fen++;
      if (c != '3' && c != '6') return false;
  }
  if (*fen++ != ' ') return false;

  // 5. Halfmove clock.
  c = *fen++;
  do {
      if (c < '0' && c > '9') return false;
  } while ((c = *fen++) != ' ');

  // 6. Fullmove number.
  c = *fen++;
  do {
      if (c < '0' || c > '9') return false;
  } while ((c = *fen++) && c != ' ');

  // End
  if (c) return false;
  return true;
}

bool insufficient_material(const Position &pos) {
  // TODO: See if more can be found
  return popcount(pos.pieces()) <= 2;
}

struct MoveInfo {
  std::string uci;
  std::string san;

  bool insufficient_material;
  bool checkmate;
  bool stalemate;
  bool zeroing;

  bool has_wdl;
  int wdl;

  bool has_dtz;
  int dtz;
};

bool compare_move_info(const MoveInfo &a, const MoveInfo &b) {
  if (a.has_dtz != b.has_dtz) return b.has_dtz;
  if (a.has_wdl != b.has_wdl) return b.has_wdl;

  if (a.has_wdl && b.has_wdl && a.wdl != b.wdl) return a.wdl < b.wdl;
  if (a.checkmate != b.checkmate) return a.checkmate;
  if (a.stalemate != b.stalemate) return a.stalemate;
  if (a.insufficient_material != b.insufficient_material) return a.insufficient_material;

  if (a.has_wdl && b.has_wdl && a.wdl < 0 && b.zeroing != a.zeroing) return a.zeroing;
  if (a.has_wdl && b.has_wdl && a.wdl > 0 && a.zeroing != b.zeroing) return b.zeroing;

  if (a.has_dtz && b.has_dtz && a.dtz != b.dtz) return b.dtz < a.dtz;

  return a.uci.compare(b.uci) < 0;
}

void get_api(struct evhttp_request *req, void *) {
  const char *uri = evhttp_request_get_uri(req);
  if (!uri) {
      std::cout << "evhttp_request_get_uri failed" << std::endl;
      return;
  }

  struct evkeyvalq *headers = evhttp_request_get_output_headers(req);
  if (cors) {
      evhttp_add_header(headers, "Access-Control-Allow-Origin", "*");
  }

  struct evkeyvalq query;
  const char *jsonp = nullptr;
  const char *c_fen = nullptr;
  if (0 == evhttp_parse_query(uri, &query)) {
      c_fen = evhttp_find_header(&query, "fen");
      jsonp = evhttp_find_header(&query, "callback");
  }
  if (!c_fen || !strlen(c_fen)) {
      evhttp_send_error(req, HTTP_BADREQUEST, "Missing FEN");
      return;
  }

  std::string fen(c_fen);
  std::replace(fen.begin(), fen.end(), '_', ' ');

  if (!validate_fen(fen.c_str())) {
      evhttp_send_error(req, HTTP_BADREQUEST, "Invalid FEN");
      return;
  }

  if (verbose) {
      std::cout << "probing: " << fen << std::endl;
  }

  Position pos;
  StateListPtr States(new std::deque<StateInfo>(1));
  pos.set(fen, false, CHESS_VARIANT, &States->back(), Threads.main());
  if (!pos.pos_is_ok()) {
      evhttp_send_error(req, HTTP_BADREQUEST, "Illegal FEN");
      return;
  }

  // Set content type
  if (jsonp && strlen(jsonp)) {
      evhttp_add_header(headers, "Content-Type", "application/javascript");
  } else {
      evhttp_add_header(headers, "Content-Type", "application/json");
  }

  // Build response
  struct evbuffer *res = evbuffer_new();
  if (!res) {
      std::cout << "could not allocate response buffer" << std::endl;
      abort();
  }

  const auto legals = MoveList<LEGAL>(pos);

  if (jsonp && strlen(jsonp)) {
      evbuffer_add_printf(res, "%s(", jsonp);
  }
  evbuffer_add_printf(res, "{\n");
  evbuffer_add_printf(res, "  \"checkmate\": %s,\n", (legals.size() == 0 && pos.checkers()) ? "true" : "false");
  evbuffer_add_printf(res, "  \"stalemate\": %s,\n", (legals.size() == 0 && !pos.checkers()) ? "true": "false");
  evbuffer_add_printf(res, "  \"moves\": [\n");

  std::vector<MoveInfo> move_infos;

  StateInfo st;
  for (const auto& m : legals) {
      pos.do_move(m, st);

      int num_moves = MoveList<LEGAL>(pos).size();

      MoveInfo info;
      info.uci = UCI::move(m, false);
      info.san = UCI::move(m, false);
      info.checkmate = num_moves == 0 && pos.checkers();
      info.stalemate = num_moves == 0 && !pos.checkers();
      info.insufficient_material = insufficient_material(pos);
      info.zeroing = pos.rule50_count() == 0;

      Tablebases::ProbeState state;
      info.dtz = Tablebases::probe_dtz(pos, &state);
      info.has_dtz = state == Tablebases::OK;

      if (info.checkmate) {
          info.has_wdl = true;
          info.wdl = -2;
      } else if (info.stalemate || info.insufficient_material) {
          info.has_wdl = true;
          info.wdl = 0;
      } else if (info.has_dtz) {
          info.has_wdl = true;
          if (info.dtz < -100 && info.dtz - pos.rule50_count() <= -100) info.wdl = -1;
          else if (info.dtz > 100 && info.dtz + pos.rule50_count() >= -100) info.wdl = 1;
          else if (info.dtz < 0) info.wdl = -2;
          else if (info.dtz > 0) info.wdl = 2;
          else info.wdl = 0;
      } else {
          info.has_wdl = false;
      }

      move_infos.push_back(info);

      pos.undo_move(m);
  }

  sort(move_infos.begin(), move_infos.end(), compare_move_info);

  for (size_t i = 0; i < move_infos.size(); i++) {
      const MoveInfo &m = move_infos[i];

      evbuffer_add_printf(res, "    {\"uci\": \"%s\", \"san\": \"%s\", \"checkmate\": %s, \"stalemate\": %s, \"insufficient_material\": %s, \"zeroing\": %s, ",
                          m.uci.c_str(), m.san.c_str(),
                          m.checkmate ? "true" : "false",
                          m.stalemate ? "true": "false",
                          m.insufficient_material ? "true": "false",
                          m.zeroing ? "true": "false");

      if (m.has_wdl) evbuffer_add_printf(res, "\"wdl\": %d, ", m.wdl);
      else evbuffer_add_printf(res, "\"wdl\": null, ");

      if (m.has_dtz) evbuffer_add_printf(res, "\"dtz\": %d}", m.dtz);
      else evbuffer_add_printf(res, "\"dtz\": null}");

      evbuffer_add_printf(res, (i + 1 < move_infos.size()) ? ",\n" : "\n");
  }

  // End response
  evbuffer_add_printf(res, "  ]\n");
  evbuffer_add_printf(res, "}");
  if (jsonp && strlen(jsonp)) evbuffer_add_printf(res, ")\n");
  else evbuffer_add_printf(res, "\n");

  evhttp_send_reply(req, HTTP_OK, "OK", res);

  evbuffer_free(res);
}

int serve(int port) {
  struct event_base *base = event_base_new();
  if (!base) {
      std::cout << "could not initialize event_base" << std::endl;
      abort();
  }

  struct evhttp *http = evhttp_new(base);
  if (!http) {
      std::cout << "could not initialize evhttp" << std::endl;
      abort();
  }

  evhttp_set_gencb(http, get_api, NULL);

  struct evhttp_bound_socket *socket = evhttp_bind_socket_with_handle(http, "127.0.0.1", port);
  if (!socket) {
      std::cout << "could not bind socket to http://127.0.0.1:" << port << std::endl;
      return 1;
  }

  std::cout << "atbserve listenning on http://127.0.0.1:" << port << " ..." << std::endl;

  return event_base_dispatch(base);
}

}  // namespace

int main(int argc, char* argv[]) {
  fclose(stdin);
  setlinebuf(stdout);

  // Options
  static int port = 5000;

  char *syzygy_path = NULL;

  // Parse command line options
  static struct option long_options[] = {
      {"verbose", no_argument,       &verbose, 1},
      {"cors",    no_argument,       &cors, 1},
      {"port",    required_argument, 0, 'p'},
      {"syzygy",  required_argument, 0, 's'},
      {NULL, 0, 0, 0},
  };

  while (true) {
      int option_index;
      int opt = getopt_long(argc, argv, "p:s:", long_options, &option_index);
      if (opt < 0) {
          break;
      }

      switch (opt) {
          case 0:
              break;

          case 'p':
              port = atoi(optarg);
              if (!port) {
                  printf("invalid port: %d\n", port);
                  return 78;
              }
              break;

          case 's':
              if (!syzygy_path) {
                  syzygy_path = strdup(optarg);
              } else {
                  syzygy_path = (char *) realloc(syzygy_path, strlen(syzygy_path) + 1 + strlen(optarg) + 1);
                  strcat(syzygy_path, ":");
                  strcat(syzygy_path, optarg);
              }
              break;

          case '?':
              return 78;

          default:
              abort();
      }
  }

  if (optind != argc) {
      std::cout << "unexpected positional argument" << std::endl;
      return 78;
  }

  if (!syzygy_path) {
     std::cout << "at least some syzygy tables are required (--syzygy)" << std::endl;
     return 78;
  }

  UCI::init(Options);
  PSQT::init();
  Bitboards::init();
  Position::init();
  Threads.init();
  Tablebases::init(syzygy_path);

  return serve(5001);
}
