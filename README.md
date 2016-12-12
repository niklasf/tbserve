Syzygy tablebase server
=======================

HTTP API for Syzygy (and Gaviota) tablebases. Based on
[@ddugovic's Stockfish with variant support](https://github.com/ddugovic/Stockfish),
and [libgtb](https://github.com/michiguel/Gaviota-Tablebases) by Miguel A. Ballicora.

Building
--------

Install libevent2:

```
sudo apt-get install build-essential libevent-dev
```

Build and install libgtb

```
git clone https://github.com/michiguel/Gaviota-Tablebases.git
cd Gaviota-Tablebases
make
sudo make install
```

Finally:

```
cd src
make -f Makefile.regular ARCH=x86-64-modern build
make -f Makefile.atomic -B ARCH=x86-64-modern build
make -f Makefile.giveaway -B ARCH=x86-64-modern build
```

Downloading tablebases
----------------------

Via BitTorrent: http://oics.olympuschess.com/tracker/index.php

Atomic and Suicide/giveaway tables are not beeing widely distributed, so they
probably have to be [generated](https://github.com/syzygy1/tb).

Usage
-----

```
./rtbserve [--verbose] [--cors] [--port 5000]
    --syzygy path/to/another/dir
    --gaviota path/to/another-dir

./atbserve [--verbose] [--cors] [--port 5000]
    --syzygy path/to/another/dir

./gtbserve [--verbose] [--cors] [--port 5000]
    --syzygy path/to/another/dir
```

HTTP API
--------

CORS enabled if `--cors` was given. Provide `callback` parameter to use JSONP.

### `GET /`

```
> curl https://tablebase.lichess.org/standard?fen=8/6B1/8/8/B7/8/K1pk4/8%20b%20-%20-%200%201
```

name | type | default | description
--- | --- | --- | ---
**fen** | string | *required* | FEN of the position to look up

```javascript
{
  "checkmate": false,
  "stalemate": false,
  "variant_win": false,
  "variant_loss": false,
  "moves": [
    {"uci": "c2c1n", "san": "c1=N+", "checkmate": false, "stalemate": false, "variant_win": false, "variant_loss": false, "insufficient_material": false, "zeroing": true, "wdl": 1, "dtz": 109, "dtm": 133},
    {"uci": "c2c1r", "san": "c1=R", "checkmate": false, "stalemate": false, "variant_win": false, "variant_loss": false, "insufficient_material": false, "zeroing": true, "wdl": 2, "dtz": 3, "dtm": 39},
    {"uci": "c2c1b", "san": "c1=B", "checkmate": false, "stalemate": false, "variant_win": false, "variant_loss": false, "insufficient_material": false, "zeroing": true, "wdl": 2, "dtz": 2, "dtm": 39},
    {"uci": "c2c1q", "san": "c1=Q", "checkmate": false, "stalemate": false, "variant_win": false, "variant_loss": false, "insufficient_material": false, "zeroing": true, "wdl": 2, "dtz": 2, "dtm": 39},
    {"uci": "d2d3", "san": "Kd3", "checkmate": false, "stalemate": false, "variant_win": false, "variant_loss": false, "insufficient_material": false, "zeroing": false, "wdl": 2, "dtz": 3, "dtm": 35},
    {"uci": "d2c1", "san": "Kc1", "checkmate": false, "stalemate": false, "variant_win": false, "variant_loss": false, "insufficient_material": false, "zeroing": false, "wdl": 2, "dtz": 5, "dtm": 31},
    {"uci": "d2d1", "san": "Kd1", "checkmate": false, "stalemate": false, "variant_win": false, "variant_loss": false, "insufficient_material": false, "zeroing": false, "wdl": 2, "dtz": 3, "dtm": 31},
    {"uci": "d2e1", "san": "Ke1", "checkmate": false, "stalemate": false, "variant_win": false, "variant_loss": false, "insufficient_material": false, "zeroing": false, "wdl": 2, "dtz": 1, "dtm": 31},
    {"uci": "d2e2", "san": "Ke2", "checkmate": false, "stalemate": false, "variant_win": false, "variant_loss": false, "insufficient_material": false, "zeroing": false, "wdl": 2, "dtz": 1, "dtm": 31},
    {"uci": "d2e3", "san": "Ke3", "checkmate": false, "stalemate": false, "variant_win": false, "variant_loss": false, "insufficient_material": false, "zeroing": false, "wdl": 2, "dtz": 1, "dtm": 31}
  ]
}
```

License
-------

tbserve is licensed under the GPLv3+. See Copying.txt for the full
license text.
