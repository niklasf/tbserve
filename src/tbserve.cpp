/*
  tbserve, a syzygy tablebase server
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

#ifdef GAVIOTA
#include <gtb-probe.h>
#endif

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

std::string move_san(Position &pos, const Move &move, const MoveList<LEGAL> &legals) {
  Square from = from_sq(move);
  Square to = to_sq(move);

  if (type_of(move) == CASTLING) {
      if (to > from) return "O-O";
      else return "O-O-O";
  }

  std::string san;

  PieceType pt = type_of(pos.piece_on(from));

  if (pt == PAWN) {
      if (file_of(from) != file_of(to)) {
          san += char('a' + file_of(from));
          san += 'x';
      }
      san += UCI::square(to);
      if (type_of(move) == PROMOTION) {
          san += '=';
          san += " PNBRQK"[promotion_type(move)];
      }
      return san;
  }

  san = " PNBRQK"[pt];

  bool rank = false, file = false;
  for (const Move &candidate : legals) {
      if (candidate == move) continue;
      if (to_sq(candidate) != to) continue;
      if (type_of(pos.piece_on(from_sq(candidate))) != pt) continue;

      if (rank_of(from_sq(candidate)) == rank_of(from)) file = true;
      if (file_of(from_sq(candidate)) == file_of(from)) rank = true;
      else file = true;
  }
  if (file) san += char('a' + file_of(from));
  if (rank) san += char('1' + rank_of(from));

  if (pos.piece_on(to)) san += 'x';
  san += UCI::square(to);
  return san;
}

template<Variant>
bool validate_kings(int wk, int bk) {
  return wk == 1 && bk == 1;
}

#ifdef ATOMIC
template<>
bool validate_kings<ATOMIC_VARIANT>(int wk, int bk) {
  return wk + bk >= 1;
}
#endif

bool validate_fen(const char *fen) {
  // 1. Board setup
  int wk = 0, bk = 0;
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
              case 'k':
                  bk++;
                  break;
              case 'K':
                  wk++;
                  break;
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
  if (!validate_kings<TABLEBASE_VARIANT>(wk, bk)) return false;

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

template<Variant>
bool insufficient_material(const Position &pos) {
  // TODO: See if more can be found
  return popcount(pos.pieces()) <= 2;
}

template<>
bool insufficient_material<CHESS_VARIANT>(const Position &pos) {
  // Easy mating material
  if (pos.pieces(PAWN) || pos.pieces(ROOK) || pos.pieces(QUEEN)) return false;

  // A single knight or a single bishop
  if (popcount(pos.pieces(KNIGHT) | pos.pieces(BISHOP)) == 1) return true;

  // More than a single knight
  if (pos.pieces(KNIGHT)) return false;

  // All bishops on the same color
  if (!(pos.pieces(BISHOP) & DarkSquares)) return true;
  else if (!(pos.pieces(BISHOP) & ~DarkSquares)) return true;
  else return false;
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

  bool has_dtm;
  int dtm;
};

bool compare_move_info(const MoveInfo &a, const MoveInfo &b) {
  if (a.has_dtz != b.has_dtz) return b.has_dtz;
  if (a.has_wdl != b.has_wdl) return b.has_wdl;

  if (a.has_wdl && b.has_wdl && a.wdl != b.wdl) return a.wdl < b.wdl;
  if (a.checkmate != b.checkmate) return a.checkmate;
  if (a.stalemate != b.stalemate) return a.stalemate;
  if (a.insufficient_material != b.insufficient_material) return a.insufficient_material;

  if (a.has_dtm && b.has_dtm && b.dtm != a.dtm) return b.dtm < a.dtm;

  if (a.has_wdl && b.has_wdl && a.wdl < 0 && b.zeroing != a.zeroing) return a.zeroing;
  if (a.has_wdl && b.has_wdl && a.wdl > 0 && a.zeroing != b.zeroing) return b.zeroing;

  if (a.has_dtz && b.has_dtz && a.dtz != b.dtz) return b.dtz < a.dtz;

  return a.uci.compare(b.uci) < 0;
}

#ifdef GAVIOTA
int probe_dtm(const Position &pos, bool *success) {
  *success = false;
  if (insufficient_material<TABLEBASE_VARIANT>(pos)) return 0;
  if (popcount(pos.pieces()) > 5) return 0;
  if (pos.can_castle(ANY_CASTLING)) return 0;

  unsigned ws[17];
  unsigned bs[17];
  unsigned char wp[17];
  unsigned char bp[17];

  unsigned i = 0;
  Bitboard white = pos.pieces(WHITE);
  while (white) {
      Square sq = pop_lsb(&white);
      ws[i] = sq;

      if (pos.piece_on(sq) == W_PAWN) wp[i] = tb_PAWN;
      else if (pos.piece_on(sq) == W_KNIGHT) wp[i] = tb_KNIGHT;
      else if (pos.piece_on(sq) == W_BISHOP) wp[i] = tb_BISHOP;
      else if (pos.piece_on(sq) == W_ROOK) wp[i] = tb_ROOK;
      else if (pos.piece_on(sq) == W_QUEEN) wp[i] = tb_QUEEN;
      else if (pos.piece_on(sq) == W_KING) wp[i] = tb_KING;
      else {
          std::cout << "inconsistent white bitboard" << std::endl;
          abort();
      }
      i++;
  }
  ws[i] = tb_NOSQUARE;
  wp[i] = tb_NOPIECE;

  i = 0;
  Bitboard black = pos.pieces(BLACK);
  while (black) {
      Square sq = pop_lsb(&black);
      bs[i] = sq;

      if (pos.piece_on(sq) == B_PAWN) bp[i] = tb_PAWN;
      else if (pos.piece_on(sq) == B_KNIGHT) bp[i] = tb_KNIGHT;
      else if (pos.piece_on(sq) == B_BISHOP) bp[i] = tb_BISHOP;
      else if (pos.piece_on(sq) == B_ROOK) bp[i] = tb_ROOK;
      else if (pos.piece_on(sq) == B_QUEEN) bp[i] = tb_QUEEN;
      else if (pos.piece_on(sq) == B_KING) bp[i] = tb_KING;
      else {
          std::cout << "inconsistent black bitboard" << std::endl;
          abort();
      }
      i++;
  }
  bs[i] = tb_NOSQUARE;
  bp[i] = tb_NOPIECE;

  unsigned info = 0;
  unsigned plies_to_mate = 0;
  unsigned available = tb_probe_hard(pos.side_to_move() == WHITE ? tb_WHITE_TO_MOVE : tb_BLACK_TO_MOVE,
                                     pos.ep_square() != SQ_NONE ? TB_squares(pos.ep_square()) : tb_NOSQUARE,
                                     0, ws, bs, wp, bp, &info, &plies_to_mate);
  if (!available || info == tb_FORBID || info == tb_UNKNOWN) {
      if (verbose) {
          std::cout << "gaviota probe failed: info = " << info << std::endl;
      }
      return 0;
  }

  if (info == tb_DRAW) return 0;

  *success = true;

  if (info == tb_WMATE && pos.side_to_move() == WHITE) {
      return plies_to_mate;
  } else if (info == tb_BMATE && pos.side_to_move() == BLACK) {
      return plies_to_mate;
  } else if (info == tb_WMATE && pos.side_to_move() == BLACK) {
      return -plies_to_mate;
  } else if (info == tb_BMATE && pos.side_to_move() == WHITE) {
      return -plies_to_mate;
  } else {
      std::cout << "gaviota tablebase error, info = " << info << std::endl;
      abort();
  }
}
#endif

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

  StateInfo states[MAX_MOVES];
  StateInfo *st = states;
  Position pos;
  pos.set(fen, true, TABLEBASE_VARIANT, st++, Threads.main());
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

#ifdef ATOMIC
  bool checkmate = legals.size() == 0 && (pos.checkers() || pos.is_atomic_loss());
#else
  bool checkmate = legals.size() == 0 && pos.checkers();
#endif

  evbuffer_add_printf(res, "{\n");
  evbuffer_add_printf(res, "  \"checkmate\": %s,\n", checkmate ? "true" : "false");
  evbuffer_add_printf(res, "  \"stalemate\": %s,\n", (legals.size() == 0 && !checkmate) ? "true": "false");
  evbuffer_add_printf(res, "  \"moves\": [\n");

  std::vector<MoveInfo> move_infos;

  for (const auto& m : legals) {
      MoveInfo info = {};
      info.uci = UCI::move(m, true);
      info.san = move_san(pos, m, legals);

      pos.do_move(m, *st++);
      int num_moves = MoveList<LEGAL>(pos).size();
#ifdef ATOMIC
      info.checkmate = num_moves == 0 && (pos.checkers() || pos.is_atomic_loss());
#else
      info.checkmate = num_moves == 0 && pos.checkers();
#endif
      info.stalemate = num_moves == 0 && !checkmate;
      info.insufficient_material = insufficient_material<TABLEBASE_VARIANT>(pos);
      info.zeroing = pos.rule50_count() == 0;

      if (info.checkmate) info.san += '#';
      else if (pos.checkers()) info.san += '+';


      if (info.checkmate) {
          info.has_wdl = true;
          info.wdl = -2;
          info.has_dtm = true;
          info.dtm = 0;
      } else if (info.stalemate || info.insufficient_material) {
          info.has_wdl = true;
          info.wdl = 0;
      } else if (!pos.can_castle(ANY_CASTLING) && popcount(pos.pieces()) <= Tablebases::MaxCardinality) {
          Tablebases::ProbeState state;
          info.dtz = Tablebases::probe_dtz(pos, &state);
          info.has_dtz = state != Tablebases::FAIL;
          if (!info.has_dtz) {
              std::cout << "dtz probe failed after " << UCI::move(m, true) << std::endl;
          } else {
              info.has_wdl = true;
              if (info.dtz < -100 && info.dtz - pos.rule50_count() <= -100) info.wdl = -1;
              else if (info.dtz > 100 && info.dtz + pos.rule50_count() >= -100) info.wdl = 1;
              else if (info.dtz < 0) info.wdl = -2;
              else if (info.dtz > 0) info.wdl = 2;
              else info.wdl = 0;

#ifdef GAVIOTA
              info.dtm = probe_dtm(pos, &info.has_dtm);
#endif
          }
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

      if (m.has_dtz) evbuffer_add_printf(res, "\"dtz\": %d", m.dtz);
      else evbuffer_add_printf(res, "\"dtz\": null");

      if (m.has_dtm) evbuffer_add_printf(res, ", \"dtm\": %d}", m.dtm);
      else evbuffer_add_printf(res, "}");

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

  std::cout << variants[TABLEBASE_VARIANT] << " tbserve listenning on http://127.0.0.1:" << port << " ..." << std::endl;

  return event_base_dispatch(base);
}

}  // namespace

int main(int argc, char* argv[]) {
  fclose(stdin);
  setlinebuf(stdout);

  // Options
  static int port = 5000;

  char *syzygy_path = NULL;

#ifdef GAVIOTA
  const char **gaviota_paths = tbpaths_init();
  if (!gaviota_paths) {
      std::cout << "tbpaths_init failed" << std::endl;
      abort();
  }
#endif

  // Parse command line options
  static struct option long_options[] = {
      {"verbose", no_argument,       &verbose, 1},
      {"cors",    no_argument,       &cors, 1},
      {"port",    required_argument, 0, 'p'},
      {"syzygy",  required_argument, 0, 's'},
#ifdef GAVIOTA
      {"gaviota", required_argument, 0, 'g'},
#endif
      {NULL, 0, 0, 0},
  };

  while (true) {
      int option_index;
#ifdef GAVIOTA
      int opt = getopt_long(argc, argv, "p:s:g:", long_options, &option_index);
#else
      int opt = getopt_long(argc, argv, "p:s:", long_options, &option_index);
#endif
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

#ifdef GAVIOTA
          case 'g':
              gaviota_paths = tbpaths_add(gaviota_paths, optarg);
              if (!gaviota_paths) {
                  std::cout << "tbpaths_add failed" << std::endl;
                  abort();
              }
              break;
#endif

          case '?':
              return 78;

          default:
              std::cout << "getopt error: " << opt << std::endl;
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

  std::cout << "SYZYGY initialization" << std::endl;

  UCI::init(Options);
  PSQT::init();
  Bitboards::init();
  Position::init();
  Threads.init();
  Tablebases::init(syzygy_path, TABLEBASE_VARIANT);

  if (Tablebases::MaxCardinality < 3) {
      std::cout << "at least some syzygy tables are required (--syzygy " << syzygy_path << ")" << std::endl;
      return 78;
  }

  std::cout << "  Path = " << syzygy_path << std::endl;
  std::cout << "  Cardinality = " << Tablebases::MaxCardinality << std::endl;
  std::cout << std::endl;

#ifdef GAVIOTA
  tbcache_init(32 * 1024 * 1024, 10);  // 32 MiB, 10% WDL
  tbstats_reset();
  char *info = tb_init(true, tb_CP4, gaviota_paths);
  if (info) {
      puts(info);
  }
#endif

  return serve(port);
}
