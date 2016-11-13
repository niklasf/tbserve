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

void get_api(struct evhttp_request *req, void *context) {
  const char *uri = evhttp_request_get_uri(req);
  if (!uri) {
      std::cout << "evhttp_request_get_uri failed" << std::endl;
      return;
  }

  struct evkeyvalq *headers = evhttp_request_get_output_headers(req);

  struct evkeyvalq query;
  const char *jsonp = nullptr;
  const char *fen = nullptr;
  if (0 == evhttp_parse_query(uri, &query)) {
      fen = evhttp_find_header(&query, "fen");
      jsonp = evhttp_find_header(&query, "callback");
  }
  if (!fen || !strlen(fen)) {
      evhttp_send_error(req, HTTP_BADREQUEST, "Missing FEN");
      return;
  }

  // TODO: Validate FEN

  Position pos;
  StateListPtr States(new std::deque<StateInfo>(1));
  pos.set(fen, false, CHESS_VARIANT, &States->back(), Threads.main());

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
  evbuffer_add_printf(res, "  \"insufficient_material\": %s,\n", insufficient_material(pos) ? "true" : "false");
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
          if (info.dtz < -100 && dtz - pos.rule50_count() <= -100) info.wdl = -1;
          else if (wdl > 100 && dtz + pos.rule50_count() >= -100) info.wdl = 1;
          else if (info.dtz < 0) info.wdl = -2;
          else if (info.dtz > 0) info.wdl = 2;
          else info.wdl = 0;
      } else {
          info.has_wdl = false;
      }

      move_infos.push_back(info);

      pos.undo_move(m);
  }

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

  UCI::init(Options);
  PSQT::init();
  Bitboards::init();
  Position::init();
  Bitbases::init();
  Search::init();
  Pawns::init();
  Threads.init();
  Tablebases::init("/home/syzygy/Downloads/syzygy");
  TT.resize(Options["Hash"]);

  return serve(5001);
}
