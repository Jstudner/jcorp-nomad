/*
 * Lozza 11 - NNUE chess engine, MIT, by Colin Jenkins. Vendored from op12no2/lozza,
 * byte-identical except that trailing whitespace was stripped (310 bytes, no
 * semantic change). Do not edit; re-vendor instead.
 *
 * Used by /Games/Chess.html as the solo opponent. It is 640 KB because the NNUE
 * weights are embedded. For scale, /assets/pdf.worker.min.js already on this card
 * is 1.06 MB, so this is not the biggest thing the device serves.
 *
 * HOW IT IS USED
 *   It speaks UCI and runs in a Web Worker, so the search never blocks the page:
 *     var w = new Worker('/assets/lozza.js');
 *     w.postMessage('uci');                      -> id/option lines, then 'uciok'
 *     w.postMessage('setoption name Hash value 8');
 *     w.postMessage('ucinewgame');
 *     w.postMessage('position fen ' + fen);      (or 'position startpos')
 *     w.postMessage('go movetime 500');
 *     w.onmessage -> 'info depth ...' lines, finally 'bestmove e2e4' | 'bestmove e7e8q'
 *
 * MEASURED (desktop, node 12 - a phone is roughly 3-5x slower):
 *     movetime 100  -> depth 5    movetime 800  -> depth 12
 *     movetime 300  -> depth 9    movetime 2000 -> depth 15
 *   movetime is HONOURED, which is why the difficulty tiers are time-based rather
 *   than depth-based: a slow phone returns on time at lower depth instead of
 *   hanging. Tactics check out - it finds a mate in one (score mate 0) and
 *   punishes 2.g4 with Qh4.
 *
 * TRAPS
 *   - MEMORY: the transposition table defaults to ~18 MB. 'setoption name Hash
 *     value 8' brings it to ~9 MB. Send it before 'ucinewgame'. Hash is a spin,
 *     min 1 max 1024. The only other option is MultiPV (1-5).
 *   - bestmove is long algebraic with an optional promotion suffix: 'e2e4',
 *     'e7e8q'. Split on whitespace, take [1], then from=slice(0,2), to=slice(2,4),
 *     promotion=slice(4) or undefined. That maps straight onto chess.js
 *     game.move({from,to,promotion}). Castling comes as 'e1g1', en passant as
 *     'e5d6'. When there is NO legal move Lozza sends `bestmove NULL ` - not the
 *     `(none)` other engines use. Every line also carries a trailing space.
 *   - STRENGTH: there is no UCI_Elo and no Skill Level; only Hash and MultiPV
 *     exist, and MultiPV is broken (it reports previous best moves, never a real
 *     top-N), so it cannot be used to weaken the bot. Use `go depth N movetime T`.
 *   - `stop` is a no-op (`case 'stop': break;`) AND the worker blocks synchronously
 *     inside go(), so a search cannot be interrupted. terminate() and respawn is the
 *     only abort; a warm respawn costs ~30 ms.
 *   - `position` and `go` are REFUSED until Hash has been set or ucinewgame sent.
 *   - Under node it needs a `performance` global (browsers have it natively, so
 *     this only bites a node harness): node 12 has no global performance.
 *   - It reads process.argv when run under node and will print "unknown command"
 *     for anything it does not recognise there. Irrelevant in a browser.
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

"use strict"

const BUILD = "11";

const BENCH_DEPTH = 8;

const INT32_MAX =  2147483647;
const INT32_MIN = -2147483648;

const NET_QA      = 255;
const NET_QB      = 64;
const NET_QAB     = NET_QA * NET_QB;
const NET_SCALE   = 400;
const NET_I_SIZE  = 768;
const NET_H1_SIZE = 256;

const IMAP = new Uint32Array(15 * 256);

const MATERIAL = new Int32Array([0,100,394,388,588,1207,10000]);
const ADJACENT = new Uint8Array(144);

const MAX_PLY         = 128;
const MAX_MOVES       = 256;
const LMR_LOOKUP      = new Uint8Array(MAX_PLY * MAX_MOVES);
const INF             = 32000;
const MATE            = 31000;
const MINMATE         = 30000;
const TTSCORE_UNKNOWN = INF + 1;
const EMPTY           = 0;

const WHITE = 0x0;
const BLACK = 0x8;

const PIECE_MASK  = 0x7;
const COLOR_MASK  = 0x8;
const COLOUR_MASK = 0x8;

const TT_EMPTY = 0;
const TT_EXACT = 1;
const TT_BETA  = 2;
const TT_ALPHA = 3;

const BASE_HASH       = INT32_MAX;
const BASE_PROMOTES   = BASE_HASH       - 1000;
const BASE_GOODTAKES  = BASE_PROMOTES   - 1000;
const BASE_EVENTAKES  = BASE_GOODTAKES  - 1000;
const BASE_EPTAKES    = BASE_EVENTAKES  - 1000;
const BASE_MATEKILLER = BASE_EPTAKES    - 1000;
const BASE_MYKILLERS  = BASE_MATEKILLER - 1000;
const BASE_GPKILLERS  = BASE_MYKILLERS  - 1000;
const BASE_CASTLING   = BASE_GPKILLERS  - 1000;
const BASE_BADTAKES   = BASE_CASTLING   - 1000;
const BASE_HISSLIDE   = 0;
const BASE_PRUNABLE   = BASE_BADTAKES;

const MOVE_TO_BITS     = 0;
const MOVE_FR_BITS     = 8;
const MOVE_TOOBJ_BITS  = 16;
const MOVE_FROBJ_BITS  = 20;
const MOVE_PROMAS_BITS = 29;

const MOVE_TO_MASK      = 0x000000FF;
const MOVE_FR_MASK      = 0x0000FF00;
const MOVE_TOOBJ_MASK   = 0x000F0000;
const MOVE_FROBJ_MASK   = 0x00F00000;
const MOVE_LEGAL_MASK   = 0x01000000;
const MOVE_EPTAKE_MASK  = 0x02000000;
const MOVE_EPMAKE_MASK  = 0x04000000;
const MOVE_CASTLE_MASK  = 0x08000000;
const MOVE_PROMOTE_MASK = 0x10000000;
const MOVE_PROMAS_MASK  = 0x60000000;  // NBRQ

const MOVE_CLEAN_MASK   = (~MOVE_LEGAL_MASK & 0xFFFFFFFF) | 0;
const MOVE_SPECIAL_MASK = MOVE_CASTLE_MASK | MOVE_PROMOTE_MASK | MOVE_EPTAKE_MASK | MOVE_EPMAKE_MASK; // need extra work in make move
const MOVE_NOISY_MASK   = MOVE_TOOBJ_MASK | MOVE_EPTAKE_MASK;

const PAWN   = 1;
const KNIGHT = 2;
const BISHOP = 3;
const ROOK   = 4;
const QUEEN  = 5;
const KING   = 6;
const EDGE   = 7;
const NO_Z   = 8;

const W_PAWN   = PAWN;
const W_KNIGHT = KNIGHT;
const W_BISHOP = BISHOP;
const W_ROOK   = ROOK;
const W_QUEEN  = QUEEN;
const W_KING   = KING;

const B_PAWN   = PAWN   | BLACK;
const B_KNIGHT = KNIGHT | BLACK;
const B_BISHOP = BISHOP | BLACK;
const B_ROOK   = ROOK   | BLACK;
const B_QUEEN  = QUEEN  | BLACK;
const B_KING   = KING   | BLACK;

//
// E === EMPTY, X = OFF BOARD, - === CANNOT HAPPEN
//
// 0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15
// E  W  W  W  W  W  W  X  -  B  B  B  B  B  B  -
// E  P  N  B  R  Q  K  X  -  P  N  B  R  Q  K  -
//

const IS_O      = new Uint8Array([0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0]);
const IS_E      = new Uint8Array([1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
const IS_OE     = new Uint8Array([1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0]);

const IS_P      = new Uint8Array([0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]);
const IS_N      = new Uint8Array([0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0]);
const IS_NBRQKE = new Uint8Array([1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 0]);
const IS_RQKE   = new Uint8Array([1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0]);
const IS_QKE    = new Uint8Array([1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0]);
const IS_K      = new Uint8Array([0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0]);
const IS_KN     = new Uint8Array([0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0]);

const IS_W      = new Uint8Array([0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
const IS_WNK    = new Uint8Array([0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
const IS_WE     = new Uint8Array([1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
const IS_WP     = new Uint8Array([0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
const IS_WN     = new Uint8Array([0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
const IS_WNBRQ  = new Uint8Array([0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
const IS_WB     = new Uint8Array([0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
const IS_WBQ    = new Uint8Array([0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
const IS_WRQ    = new Uint8Array([0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
const IS_WQ     = new Uint8Array([0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);

const IS_B      = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0]);
const IS_BNK    = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0]);
const IS_BE     = new Uint8Array([1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0]);
const IS_BP     = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]);
const IS_BN     = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0]);
const IS_BNBRQ  = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0]);
const IS_BB     = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0]);
const IS_BBQ    = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0]);
const IS_BRQ    = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0]);
const IS_BQ     = new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0]);

const W_PROMOTE_SQ = new Uint8Array([0,26, 27, 28, 29, 30, 31, 32, 33]);
const B_PROMOTE_SQ = new Uint8Array([0,110,111,112,113,114,115,116,117]);

const A1 = 110, B1 = 111, C1 = 112, D1 = 113, E1 = 114, F1 = 115, G1 = 116, H1 = 117;
const A8 = 26,  B8 = 27,  C8 = 28,  D8 = 29,  E8 = 30,  F8 = 31,  G8 = 32,  H8 = 33;

const SQA1 = 110, SQB1 = 111, SQC1 = 112, SQD1 = 113, SQE1 = 114, SQF1 = 115, SQG1 = 116, SQH1 = 117;
const SQA2 = 98,  SQB2 = 99,  SQC2 = 100, SQD2 = 101, SQE2 = 102, SQF2 = 103, SQG2 = 104, SQH2 = 105;
const SQA3 = 86,  SQB3 = 87,  SQC3 = 88,  SQD3 = 89,  SQE3 = 90,  SQF3 = 91,  SQG3 = 92,  SQH3 = 93;
const SQA4 = 74,  SQB4 = 75,  SQC4 = 76,  SQD4 = 77,  SQE4 = 78,  SQF4 = 79,  SQG4 = 80,  SQH4 = 81;
const SQA5 = 62,  SQB5 = 63,  SQC5 = 64,  SQD5 = 65,  SQE5 = 66,  SQF5 = 67,  SQG5 = 68,  SQH5 = 69;
const SQA6 = 50,  SQB6 = 51,  SQC6 = 52,  SQD6 = 53,  SQE6 = 54,  SQF6 = 55,  SQG6 = 56,  SQH6 = 57;
const SQA7 = 38,  SQB7 = 39,  SQC7 = 40,  SQD7 = 41,  SQE7 = 42,  SQF7 = 43,  SQG7 = 44,  SQH7 = 45;
const SQA8 = 26,  SQB8 = 27,  SQC8 = 28,  SQD8 = 29,  SQE8 = 30,  SQF8 = 31,  SQG8 = 32,  SQH8 = 33;

const MOVE_E1G1 = MOVE_CASTLE_MASK | (W_KING << MOVE_FROBJ_BITS) | (E1 << MOVE_FR_BITS) | G1;
const MOVE_E1C1 = MOVE_CASTLE_MASK | (W_KING << MOVE_FROBJ_BITS) | (E1 << MOVE_FR_BITS) | C1;
const MOVE_E8G8 = MOVE_CASTLE_MASK | (B_KING << MOVE_FROBJ_BITS) | (E8 << MOVE_FR_BITS) | G8;
const MOVE_E8C8 = MOVE_CASTLE_MASK | (B_KING << MOVE_FROBJ_BITS) | (E8 << MOVE_FR_BITS) | C8;

const QPRO = (QUEEN-2)  << MOVE_PROMAS_BITS | MOVE_PROMOTE_MASK;
const RPRO = (ROOK-2)   << MOVE_PROMAS_BITS | MOVE_PROMOTE_MASK;
const BPRO = (BISHOP-2) << MOVE_PROMAS_BITS | MOVE_PROMOTE_MASK;
const NPRO = (KNIGHT-2) << MOVE_PROMAS_BITS | MOVE_PROMOTE_MASK;

const WHITE_RIGHTS_KING  = 0x00000001;
const WHITE_RIGHTS_QUEEN = 0x00000002;
const BLACK_RIGHTS_KING  = 0x00000004;
const BLACK_RIGHTS_QUEEN = 0x00000008;
const WHITE_RIGHTS       = WHITE_RIGHTS_QUEEN | WHITE_RIGHTS_KING;
const BLACK_RIGHTS       = BLACK_RIGHTS_QUEEN | BLACK_RIGHTS_KING;

const MASK_RIGHTS = new Int8Array([
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, ~8, 15, 15, 15, ~12,15, 15, ~4, 15, 15,
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, ~2, 15, 15, 15, ~3, 15, 15, ~1, 15, 15,
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15
]);

const RANK_VECTOR = new Uint8Array([0,1,2,2,4,5,6]);  // for move sorting

const B88 = new Uint8Array([
  26, 27, 28, 29, 30, 31, 32, 33,
  38, 39, 40, 41, 42, 43, 44, 45,
  50, 51, 52, 53, 54, 55, 56, 57,
  62, 63, 64, 65, 66, 67, 68, 69,
  74, 75, 76, 77, 78, 79, 80, 81,
  86, 87, 88, 89, 90, 91, 92, 93,
  98, 99, 100,101,102,103,104,105,
  110,111,112,113,114,115,116,117
]);

const COORDS = [
  '??', '??', '??', '??', '??', '??', '??', '??', '??', '??', '??', '??',
  '??', '??', '??', '??', '??', '??', '??', '??', '??', '??', '??', '??',
  '??', '??', 'a8', 'b8', 'c8', 'd8', 'e8', 'f8', 'g8', 'h8', '??', '??',
  '??', '??', 'a7', 'b7', 'c7', 'd7', 'e7', 'f7', 'g7', 'h7', '??', '??',
  '??', '??', 'a6', 'b6', 'c6', 'd6', 'e6', 'f6', 'g6', 'h6', '??', '??',
  '??', '??', 'a5', 'b5', 'c5', 'd5', 'e5', 'f5', 'g5', 'h5', '??', '??',
  '??', '??', 'a4', 'b4', 'c4', 'd4', 'e4', 'f4', 'g4', 'h4', '??', '??',
  '??', '??', 'a3', 'b3', 'c3', 'd3', 'e3', 'f3', 'g3', 'h3', '??', '??',
  '??', '??', 'a2', 'b2', 'c2', 'd2', 'e2', 'f2', 'g2', 'h2', '??', '??',
  '??', '??', 'a1', 'b1', 'c1', 'd1', 'e1', 'f1', 'g1', 'h1', '??', '??',
  '??', '??', '??', '??', '??', '??', '??', '??', '??', '??', '??', '??',
  '??', '??', '??', '??', '??', '??', '??', '??', '??', '??', '??', '??'
];

const NAMES    = ['-','P','N','B','R','Q','K','-'];
const PROMOTES = ['n','b','r','q'];                  // 0-3 encoded in move

const RANK = new Uint8Array([
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 8, 8, 8, 8, 8, 8, 8, 8, 0, 0,
  0, 0, 7, 7, 7, 7, 7, 7, 7, 7, 0, 0,
  0, 0, 6, 6, 6, 6, 6, 6, 6, 6, 0, 0,
  0, 0, 5, 5, 5, 5, 5, 5, 5, 5, 0, 0,
  0, 0, 4, 4, 4, 4, 4, 4, 4, 4, 0, 0,
  0, 0, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0,
  0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0,
  0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
]);

const FILE = new Uint8Array([
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0,
  0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0,
  0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0,
  0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0,
  0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0,
  0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0,
  0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0,
  0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]);
  
const NULL144 = new Uint8Array(144);
  
const MAP = Object.seal({
  'p': B_PAWN,
  'n': B_KNIGHT,
  'b': B_BISHOP,
  'r': B_ROOK,
  'q': B_QUEEN,
  'k': B_KING,
  'P': W_PAWN,
  'N': W_KNIGHT,
  'B': W_BISHOP,
  'R': W_ROOK,
  'Q': W_QUEEN,
  'K': W_KING
});
  
const UMAP = Object.seal({
  9:  'p',
  10: 'n',
  11: 'b',
  12: 'r',
  13: 'q',
  14: 'k',
  1:  'P',
  2:  'N',
  3:  'B',
  4:  'R',
  5:  'Q',
  6:  'K'
});
  
const ALIGNED = Array(144);

let WEIGHTS_B64;

function seal (o) {
  Object.seal(o);
}

function myround(x) {
  return Math.sign(x) * Math.round(Math.abs(x));
}

function now() {
  return performance.now() | 0;
}

const nodes = Array(MAX_PLY);

function nodeStruct () {

  this.ply             = 0;
  this.childNode       = null;
  this.parentNode      = null;
  this.grandparentNode = null;

  this.moves  = new Uint32Array(MAX_MOVES);
  this.ranks  = new Int32Array(MAX_MOVES);
  this.moves2 = new Uint32Array(MAX_MOVES);
  this.ranks2 = new Int32Array(MAX_MOVES);

  this.killer1     = 0;
  this.killer2     = 0;
  this.mateKiller  = 0;
  this.stage       = 0;
  this.numMoves    = 0;
  this.numMoves2   = 0;
  this.next        = 0;
  this.hashMove    = 0;
  this.hashEval    = 0;
  this.base        = 0;
  this.inCheck     = 0;
  this.ev          = 0;

  this.rights = 0;
  this.ep     = 0;
  this.repLo  = 0;
  this.repHi  = 0;
  this.loHash = 0;
  this.hiHash = 0;

  this.net_h1_a  = new Int32Array(NET_H1_SIZE);
  this.net_h2_a  = new Int32Array(NET_H1_SIZE);
  this.net_a     = [[this.net_h1_a, this.net_h2_a], [this.net_h2_a, this.net_h1_a]];
  this.accsDirty = 0;

  this.toZ = 0;
  this.frZ = 0;
  this.epZ = 0;

  this.pv    = new Uint32Array(MAX_MOVES);
  this.pvLen = 0;

}


// initNode

function initNode (node) {

  node.killer1    = 0;
  node.killer2    = 0;
  node.mateKiller = 0;
  node.numMoves   = 0;
  node.next       = 0;
  node.hashMove   = 0;
  node.base       = 0;
  node.inCheck    = 0;

  node.toZ = 0;
  node.frZ = 0;
  node.epZ = 0;

  node.accsDirty = 0;

}

// cache
//
// The accumulators are not cached/uncached; each node owns the
// accumulators for its position, written lazily by resolveAccs()
// in net.js.
//

function cache (node) {

  node.rights = bdRights;
  node.ep     = bdEp;
  node.repLo  = repLo;
  node.repHi  = repHi;
  node.loHash = loHash;
  node.hiHash = hiHash;

}

// uncacheA

function uncacheA (node) {

  bdRights = node.rights;
  bdEp     = node.ep;
  repLo    = node.repLo;
  repHi    = node.repHi;
  loHash   = node.loHash;
  hiHash   = node.hiHash;

}

function getNextMove (node) {

  switch (node.stage) {

    case 0: {
      // node.moves
      
      if (node.next !== node.numMoves) {
      
        let maxM = 0;
      
        const moves = node.moves;
        const ranks = node.ranks;
        const next  = node.next;
        const num   = node.numMoves;
      
        let maxR = INT32_MIN;
        let maxI = 0;
      
        for (let i=next; i < num; i++) {
          if (ranks[i] > maxR) {
            maxR = ranks[i];
            maxI = i;
          }
        }
      
        maxM = moves[maxI]
      
        moves[maxI] = moves[next];
        ranks[maxI] = ranks[next];
      
        node.base = maxR;
      
        node.next++;
      
        return maxM;
      
      }
      
      else {
      
        node.stage++;
        node.next = 0;
      
        rankSlides(node);
      
      }
      
    }

    case 1: {
      // node.moves2
      
      if (node.next !== node.numMoves2) {
      
        let maxM = 0;
      
        const moves = node.moves2;
        const ranks = node.ranks2;
        const next  = node.next;
        const num   = node.numMoves2;
      
        let maxR = INT32_MIN;
        let maxI = 0;
      
        for (let i=next; i < num; i++) {
          if (ranks[i] > maxR) {
            maxR = ranks[i];
            maxI = i;
          }
        }
      
        maxM = moves[maxI]
      
        moves[maxI] = moves[next];
        ranks[maxI] = ranks[next];
      
        node.base = maxR;
      
        node.next++;
      
        return maxM;
      
      }
      
      else {
      
        return 0;
      
      }
      
    }

  }
}

function rankSlides (node) {

  for (let i=0; i < node.numMoves2; i++) {

    const move  = node.moves2[i];
    const to    = (move & MOVE_TO_MASK)    >>> MOVE_TO_BITS;
    const frObj = (move & MOVE_FROBJ_MASK) >>> MOVE_FROBJ_BITS;

    const hisScore = objHistory[(frObj << 8) + to];

    node.ranks2[i] = hisScore;

  }
}

function addSlide (node, move) {

  const m = move & MOVE_CLEAN_MASK;

  if (m === node.hashMove) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_HASH;
  }

  else if (m === node.mateKiller) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_MATEKILLER;
  }

  else if (m === node.killer1) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_MYKILLERS + 1;
  }

  else if (m === node.killer2) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_MYKILLERS;
  }

  else if (node.grandparentNode !== null && m === node.grandparentNode.killer1) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_GPKILLERS + 1;
  }

  else if (node.grandparentNode !== null && m === node.grandparentNode.killer2) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_GPKILLERS;
  }

  else {
    node.moves2[node.numMoves2++] = move;  // defer ranking until later
  }

}

function addCastle (node, move) {

  const m = move & MOVE_CLEAN_MASK;

  if (m === node.hashMove) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_HASH;
  }

  else if (m === node.mateKiller) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_MATEKILLER;
  }

  else if (m === node.killer1) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_MYKILLERS + 1;
  }

  else if (m === node.killer2) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_MYKILLERS;
  }

  else if (node.grandparentNode !== null && m === node.grandparentNode.killer1) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_GPKILLERS + 1;
  }

  else if (node.grandparentNode !== null && m === node.grandparentNode.killer2) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_GPKILLERS;
  }

  else {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_CASTLING;
  }

}

function addCapture (node, move) {

  const m = move & MOVE_CLEAN_MASK;

  if (m === node.hashMove) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_HASH;
  }

  else {

    const victim = RANK_VECTOR[((move & MOVE_TOOBJ_MASK) >>> MOVE_TOOBJ_BITS) & PIECE_MASK];
    const attack = RANK_VECTOR[((move & MOVE_FROBJ_MASK) >>> MOVE_FROBJ_BITS) & PIECE_MASK];

    if (victim > attack) {
      node.moves[node.numMoves]   = move;
      node.ranks[node.numMoves++] = BASE_GOODTAKES + (victim << 6) - attack;
    }

    else if (victim === attack) {
      node.moves[node.numMoves]   = move;
      node.ranks[node.numMoves++] = BASE_EVENTAKES + (victim << 6) - attack;
    }

    else {

      if (m === node.mateKiller) {
        node.moves[node.numMoves]   = move;
        node.ranks[node.numMoves++] = BASE_MATEKILLER;
      }

      else if (m === node.killer1) {
        node.moves[node.numMoves]   = move;
        node.ranks[node.numMoves++] = BASE_MYKILLERS + 1;
      }

      else if (m === node.killer2) {
        node.moves[node.numMoves]   = move;
        node.ranks[node.numMoves++] = BASE_MYKILLERS;
      }

      else if (node.grandparentNode !== null && m === node.grandparentNode.killer1) {
        node.moves[node.numMoves]   = move;
        node.ranks[node.numMoves++] = BASE_GPKILLERS + 1;
      }

      else if (node.grandparentNode !== null && m === node.grandparentNode.killer2) {
        node.moves[node.numMoves]   = move;
        node.ranks[node.numMoves++] = BASE_GPKILLERS;
      }

      else {
        node.moves[node.numMoves]   = move;
        node.ranks[node.numMoves++] = BASE_BADTAKES + (victim << 6) - attack;
      }

    }
  }
}

function addPromotion (node, move) {

  const m = move & MOVE_CLEAN_MASK;

  if ((m | QPRO) === node.hashMove) {
    node.moves[node.numMoves]   = move | QPRO;
    node.ranks[node.numMoves++] = BASE_HASH;
  }
  else {
    node.moves[node.numMoves]   = move | QPRO;
    node.ranks[node.numMoves++] = BASE_PROMOTES + QUEEN;
  }

  if ((m | RPRO) === node.hashMove) {
    node.moves[node.numMoves]   = move | RPRO;
    node.ranks[node.numMoves++] = BASE_HASH;
  }
  else {
    node.moves[node.numMoves]   = move | RPRO;
    node.ranks[node.numMoves++] = BASE_PROMOTES + ROOK;
  }

  if ((m | BPRO) === node.hashMove) {
    node.moves[node.numMoves]   = move | BPRO;
    node.ranks[node.numMoves++] = BASE_HASH;
  }
  else {
    node.moves[node.numMoves]   = move | BPRO;
    node.ranks[node.numMoves++] = BASE_PROMOTES + BISHOP;
  }

  if ((m | NPRO) === node.hashMove) {
    node.moves[node.numMoves]   = move | NPRO;
    node.ranks[node.numMoves++] = BASE_HASH;
  }
  else {
    node.moves[node.numMoves]   = move | NPRO;
    node.ranks[node.numMoves++] = BASE_PROMOTES + KNIGHT;
  }

}

function addEPTake (node, move) {

  const m = move & MOVE_CLEAN_MASK;

  if ((m | MOVE_EPTAKE_MASK) === node.hashMove) {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_HASH;
  }

  else {
    node.moves[node.numMoves]   = move;
    node.ranks[node.numMoves++] = BASE_EPTAKES;
  }

}

function addQMove (node, move) {

  const m = move & MOVE_CLEAN_MASK;
  const n = node.numMoves++;

  node.moves[n] = move;

  if (m === node.hashMove)
    node.ranks[n] = BASE_HASH;

  else if ((move & MOVE_PROMOTE_MASK) !== 0) {
    if ((move & MOVE_NOISY_MASK) !== 0)
      node.ranks[n] = BASE_PROMOTES + ((move & MOVE_PROMAS_MASK) >>> MOVE_PROMAS_BITS) + 8;  // QRBN capture
    else
      node.ranks[n] = BASE_PROMOTES + ((move & MOVE_PROMAS_MASK) >>> MOVE_PROMAS_BITS);      // QRBN slide (allowed in qs)
  }

  else if ((move & MOVE_EPTAKE_MASK) !== 0)
    node.ranks[n] = BASE_EPTAKES;

  else {
    const victim = RANK_VECTOR[((move & MOVE_TOOBJ_MASK) >>> MOVE_TOOBJ_BITS) & PIECE_MASK];
    const attack = RANK_VECTOR[((move & MOVE_FROBJ_MASK) >>> MOVE_FROBJ_BITS) & PIECE_MASK];

    if (victim > attack)
      node.ranks[n] = BASE_GOODTAKES + (victim << 6) - attack;

    else if (victim === attack)
      node.ranks[n] = BASE_EVENTAKES + (victim << 6) - attack;

    else
      node.ranks[n] = BASE_BADTAKES + (victim << 6) - attack;
  }

}

function addQPromotion (node, move) {

  addQMove (node, move | (QUEEN-2)  << MOVE_PROMAS_BITS);
  addQMove (node, move | (ROOK-2)   << MOVE_PROMAS_BITS);
  addQMove (node, move | (BISHOP-2) << MOVE_PROMAS_BITS);
  addQMove (node, move | (KNIGHT-2) << MOVE_PROMAS_BITS);

}

function addKiller (node, score, move) {

  move = move & MOVE_CLEAN_MASK;

  if (move === node.hashMove)
    return;

  if ((move & (MOVE_EPTAKE_MASK | MOVE_PROMOTE_MASK)) !== 0)
    return;  // before killers in move ordering.

  if ((move & MOVE_TOOBJ_MASK) !== 0) {

    const victim = RANK_VECTOR[((move & MOVE_TOOBJ_MASK) >>> MOVE_TOOBJ_BITS) & PIECE_MASK];
    const attack = RANK_VECTOR[((move & MOVE_FROBJ_MASK) >>> MOVE_FROBJ_BITS) & PIECE_MASK];

    if (victim >= attack)
      return; // before killers in move ordering.
  }

  if (score >= MINMATE && score <= MATE) {
    node.mateKiller = move;
    if (node.killer1 === move)
      node.killer1 = 0;
    if (node.killer2 === move)
      node.killer2 = 0;
    return;
  }

  if (node.killer1 === move || node.killer2 === move) {
    return;
  }

  if (node.killer1 === 0) {
    node.killer1 = move;
    return;
  }

  if (node.killer2 === 0) {
    node.killer2 = move;
    return;
  }

  const tmp    = node.killer1;
  node.killer1 = move;
  node.killer2 = tmp;

}

function go (maxPly) {

  let bestMoveStr = '';
  let alpha       = 0;
  let beta        = 0;
  let score       = 0;
  let delta       = 0;
  let depth       = 0;
  let reported    = 0;

  multiPVMoves = [];

  for (let ply=1; ply <= maxPly; ply++) {
    // id
    
    alpha = -INF;
    beta  = INF;
    delta = 10;
    depth = ply;
    
    if (ply >= 4) {
      alpha = Math.max(-INF, score - delta);
      beta  = Math.min(INF,  score + delta);
    }
    
    while (1) {
      // asp
      
      score = rootSearch(rootNode, depth, bdTurn, alpha, beta);
      
      if (statsTimeOut)
        break;
      
      delta += delta/2 | 0;
      
      if (score <= alpha) {
        // upper bound

        beta  = Math.min(INF, ((alpha + beta) / 2) | 0);
        alpha = Math.max(-INF, alpha - delta);

        report(score, depth, 'upperbound');
        reported = 1;

        if (!statsMaxNodes)
          statsBestMove = 0;

      }

      else if (score >= beta) {
        // lower bound

        beta = Math.min(INF, beta + delta);

        report(score, depth, 'lowerbound');
        reported = 1;

        depth = Math.max(1, depth-1);

      }

      else {
        // exact

        // track for MultiPV

        if (statsBestMove) {
          multiPVMoves = multiPVMoves.filter(m => m.move !== statsBestMove);
          multiPVMoves.unshift({
            move:  statsBestMove,
            score: score,
            pv:    rootNode.pv.slice(0, rootNode.pvLen),
            pvLen: rootNode.pvLen
          });
        }


        if (multiPV > 1)
          reportMultiPV(depth);
        else
          report(score, depth, '');

        reported = 1;

        if (statsBestMove && statsMaxNodes > 0 && statsNodes >= statsMaxNodes)
          statsTimeOut = 1;

        break;

      }
      
    }
    
    if (statsTimeOut)
      break;
    
  }

  if (reported === 0)
    report(statsBestScore, depth || 1, '');

  bestMoveStr = formatMove(statsBestMove);

  uciSend('bestmove', bestMoveStr);

}

function newGame() {

  if (ttSize == 1)
    ttResize(ttDefault);

  ttInit();

}

function rootSearch (node, depth, turn, alpha, beta) {

  // check time
  
  node.pvLen = 0;
  
  if (node.childNode === null) {
    statsTimeOut = 1;
    return 0;
  }

  statsNodes++;

  const nextTurn = turn ^ COLOR_MASK;
  const oAlpha   = alpha;
  const inCheck  = isKingAttacked(nextTurn);
  const doLMR    = (depth >= 3) | 0;

  let numLegalMoves = 0;
  let numPrunes     = 0;
  let move          = 0;
  let bestMove      = 0;
  let score         = 0;
  let bestScore     = -INF;
  let R             = 0;
  let E             = 0;

  score = ttGet(node, depth, alpha, beta);  // load hash move and hash eval

  node.inCheck = inCheck;
  node.ev      = node.hashEval !== INF ? node.hashEval : evaluate(node, turn);

  ttUpdateEval(node.ev);
  cache(node);
  genMoves(node, turn);

  while ((move = getNextMove(node)) !== 0) {

    makeMoveA(node, move);

    // legal?

    if ((move & MOVE_LEGAL_MASK) === 0 && isKingAttacked(nextTurn) !== 0) {

      unmakeMove(node, move);

      uncacheA(node);

      continue;

    }

    numLegalMoves++;
    if (node.base <= BASE_PRUNABLE)
      numPrunes++;

    // send current move to UCI?
    
    if (statsNodes > 10000000)
      uciSend('info currmove ' + formatMove(move) + ' currmovenumber ' + numLegalMoves);
    
    // extend/reduce
    
    E = 0;
    R = 0;
    
    if (inCheck !== 0) {
      E = 1;
    }
    
    else if (doLMR !== 0 && numLegalMoves > 4) {
      R = LMR_LOOKUP[(depth << 8) + numPrunes];
    }

    const nullWindow = (numLegalMoves > 1 || R) | 0;

    score = alpha;

    if (nullWindow !== 0)
      score = -search(node.childNode, depth+E-R-1, nextTurn, -alpha-1, -alpha);

    if (statsTimeOut === 0 && (nullWindow === 0 || score > alpha))
      score = -search(node.childNode, depth+E-1, nextTurn, -beta, -alpha);

    // unmake move

    unmakeMove(node, move);

    uncacheA(node);

    if (statsTimeOut !== 0)
      return 0;

    if (score > bestScore) {

      bestScore = score;
      bestMove  = move;

      if (bestScore > alpha) {

        collectPV(node, move);

        alpha = bestScore;

        statsBestMove  = bestMove;
        statsBestScore = bestScore;

        if (bestScore >= beta) {
          addKiller(node, bestScore, bestMove);
          if ((move & MOVE_NOISY_MASK) === 0)
            addHistory(Math.imul(Math.imul(depth, depth), depth), bestMove);
          ttPut(TT_BETA, depth, bestScore, bestMove, node.ply, alpha, beta, INF);
          return bestScore;
        }

        else {
          if ((move & MOVE_NOISY_MASK) === 0)
            addHistory(Math.imul(depth, depth), bestMove);
        }
      }
    }

    else {
      if ((move & MOVE_NOISY_MASK) === 0)
        addHistory(-depth, move);
    }
  }

  // update tt etc
  
  if (numLegalMoves === 1)
    statsTimeOut = 1;  // only one legal move so don't waste any more time
  
  if (numLegalMoves === 0) {
    statsTimeOut = 1;  // silly position
    statsBestMove = 0;
    statsBestScore = 0;
  }
  
  if (bestScore > oAlpha) {
    ttPut(TT_EXACT, depth, bestScore, bestMove, node.ply, alpha, beta, node.ev);
    return bestScore;
  }
  else {
    ttPut(TT_ALPHA, depth, bestScore, bestMove, node.ply, alpha, beta, node.ev);
    return bestScore;
  }
  
}

// search

function search (node, depth, turn, alpha, beta) {

  // check time
  
  node.pvLen = 0;
  
  if (node.childNode === null) {
    statsTimeOut = 1;
    return 0;
  }
  
  checkTime();
  if (statsTimeOut !== 0)
    return 0;
  
  if (node.ply > statsSelDepth)
    statsSelDepth = node.ply;
  
  const nextTurn = turn ^ COLOR_MASK;
  const pvNode   = (beta !== (alpha + 1)) | 0;

  // mate distance pruning
  
  const matingValue1 = MATE - node.ply;
  
  if (matingValue1 < beta) {
     beta = matingValue1;
     if (alpha >= matingValue1)
       return matingValue1;
  }
  
  const matingValue2 = -MATE + node.ply;
  
  if (matingValue2 > alpha) {
     alpha = matingValue2;
     if (beta <= matingValue2)
       return matingValue2;
  }
  
  // check for draws
  
  if (isDraw() !== 0)
    return 0;
  
  const inCheck = isKingAttacked(nextTurn);

  // horizon
  
  if (inCheck === 0 && depth <= 0)
    return qSearch(node, -1, turn, alpha, beta);

  statsNodes++;
  
  depth = Math.max(depth,0);
  

  let score = 0;

  // try tt
  
  score = ttGet(node, depth, alpha, beta);  // sets/clears node.hashMove and node.hashEval
  
  if (pvNode === 0 && score !== TTSCORE_UNKNOWN)
    return score;

  const doBeta = ((pvNode === 0 && inCheck === 0 && beta < MINMATE)) | 0;

  let R = 0;
  let E = 0;

  const ev = node.hashEval !== INF ? node.hashEval : evaluate(node, turn);

  // improving
  
  var improving = 0;
  
  if (inCheck === 0) {
    const n2 = node.grandparentNode;
    if (n2 !== null) {
      if (n2.inCheck === 0 && ev > n2.ev)
        improving = 1;
      else if (n2.inCheck !== 0) {
        const n4 = n2.grandparentNode;
        if (n4 !== null && n4.inCheck === 0 && ev > n4.ev)
          improving = 1;
      }
    }
  }
  
  // beta prune
  
  if (doBeta !== 0 && depth <= 8 && (ev - Math.imul(depth, 100)) >= (beta - Math.imul(improving, 50)))
    return ev;
  
  // alpha prune
  
  // hack if (pvNode == 0 && inCheck === 0 && alpha > -MINMATE && depth <= 4 && (ev + 900 * depth) <= alpha) {
    //const qs = qSearch(node, -1, turn, alpha, alpha + 1);
    //if (qs <= alpha) {
      //return qs;
    //}
  //}

  node.inCheck = inCheck;
  node.ev      = ev;

  resolveAccs(node);  // may not have happened via evaluate(); children resolve from our accumulators

  cache(node);

  // NMP

  //const isPawnEG = (wCount == wCounts[PAWN]+1 && bCount == bCounts[PAWN]+1) | 0;

  if (doBeta !== 0 && depth > 2 && ev > beta) {

    R = 3 + improving;

    loHash ^= loEP[bdEp];
    hiHash ^= hiEP[bdEp];

    bdEp = 0;

    loHash ^= loEP[bdEp];
    hiHash ^= hiEP[bdEp];

    loHash ^= loTurn;
    hiHash ^= hiTurn;

    repLo = repHi;

    node.childNode.net_h1_a.set(node.net_h1_a);  // null move; same position for the child
    node.childNode.net_h2_a.set(node.net_h2_a);
    node.childNode.accsDirty = 0;

    score = -search(node.childNode, depth-R-1, nextTurn, -beta, -beta+1);

    uncacheA(node);

    if (score >= beta) {
      if (score > MINMATE)
        score = beta;
      return score;
    }
  
    if (statsTimeOut !== 0)
      return 0;
  }
  

  const oAlpha = alpha;
  const doFP   = (inCheck === 0 && depth <= 4) | 0;
  const doLMR  = (inCheck === 0 && depth >= 3) | 0;
  const doLMP  = (pvNode === 0 && inCheck === 0 && depth <= 2) | 0;
  const doIIR  = (inCheck === 0 && node.hashMove === 0 && depth > 3) | 0;
  
  let bestScore     = -INF;
  let move          = 0;
  let bestMove      = 0;
  let numLegalMoves = 0;
  let numPrunes     = 0;

  // IIR
  //
  // https://www.talkchess.com/forum3/viewtopic.php?f=7&t=74769
  //
  
  if (doIIR !== 0) {
  
    depth -= 1;
  
  }
  
  ttUpdateEval(ev);
  genMoves(node, turn);

  while ((move = getNextMove(node)) !== 0) {

    // prune
    
    const prune = (numLegalMoves > 0 && node.base <= BASE_PRUNABLE && alpha > -MINMATE) | 0;
    
    if (doLMP !== 0 && prune !== 0 && numPrunes > Math.imul(depth, 5))
      continue;
    
    if (doFP !== 0 && prune !== 0 && (ev + Math.imul(depth, 120)) < alpha)
      continue;

    makeMoveA(node, move);

    // legal

    if ((move & MOVE_LEGAL_MASK) === 0 && isKingAttacked(nextTurn) !== 0) {

      unmakeMove(node, move);

      uncacheA(node);

      continue;

    }

    numLegalMoves++;
    if (node.base <= BASE_PRUNABLE)
      numPrunes++;

    // extend/reduce
    
    E = 0;
    R = 0;
    
    if (inCheck !== 0 && (pvNode !== 0 || depth < 5)) {
      E = 1;
    }
    
    else if (doLMR !== 0 && numLegalMoves > 4) {
      R = LMR_LOOKUP[(depth << 8) + numPrunes];
    }
    
    const nullWindow = ((pvNode !== 0 && numLegalMoves > 1) || R) | 0;

    score = alpha;

    if (nullWindow !== 0)
      score = -search(node.childNode, depth+E-R-1, nextTurn, -alpha-1, -alpha);

    if (statsTimeOut === 0 && (nullWindow === 0 || score > alpha))
      score = -search(node.childNode, depth+E-1, nextTurn, -beta, -alpha);

    // unmake move

    unmakeMove(node, move);

    uncacheA(node);

    if (statsTimeOut !== 0)
      return 0;

    if (score > bestScore) {

      bestScore = score;
      bestMove  = move;

      if (bestScore > alpha) {

        if (pvNode !== 0)
          collectPV(node, move);

        alpha = bestScore;

        if (bestScore >= beta) {
          addKiller(node, bestScore, bestMove);
          if ((move & MOVE_NOISY_MASK) === 0)
            addHistory(Math.imul(Math.imul(depth, depth), depth), bestMove);
          ttPut(TT_BETA, depth, bestScore, bestMove, node.ply, alpha, beta, ev);
          return bestScore;
        }

        else {
          if ((move & MOVE_NOISY_MASK) === 0)
            addHistory(Math.imul(depth, depth), bestMove);
        }
      }
    }

    else {
      if ((move & MOVE_NOISY_MASK) === 0)
        addHistory(-depth, move);
    }
  }

  // mate
  
  if (numLegalMoves === 0) {
  
    if (inCheck !== 0) {
      //ttPut(TT_EXACT, depth, -MATE + node.ply, 0, node.ply, alpha, beta, ev);
      return -MATE + node.ply;
    }
  
    else {
      //ttPut(TT_EXACT, depth, 0, 0, node.ply, alpha, beta, ev);
      return 0;
    }
  
  }

  if (bestScore > oAlpha) {
    ttPut(TT_EXACT, depth, bestScore, bestMove, node.ply, alpha, beta, ev);
    return bestScore;
  }
  else {
    ttPut(TT_ALPHA, depth, bestScore, bestMove, node.ply, alpha, beta, ev);
    return bestScore;
  }

}

function qSearch (node, depth, turn, alpha, beta) {

  // check depth
  
  node.pvLen = 0;

  statsNodes++;
  
  if (node.ply > statsSelDepth)
    statsSelDepth = node.ply;

  if (node.childNode === null)
    return evaluate(node, turn);
  
  const nextTurn = turn ^ COLOR_MASK;

  if (isDraw() !== 0)
    return 0;

  let score = ttGet(node, 0, alpha, beta);  // sets/clears node.hashMove and node.hashEval

  if (score !== TTSCORE_UNKNOWN)
    return score;

  const ev = node.hashEval !== INF ? node.hashEval : evaluate(node, turn);

  if (ev >= beta)
    return ev;
  if (ev >= alpha)
    alpha = ev;

  node.inCheck = 0;  // but not used

  resolveAccs(node);  // may not have happened via evaluate(); children resolve from our accumulators

  ttUpdateEval(ev);
  cache(node);
  genQMoves(node, turn);

  let numLegalMoves = 0;
  let move          = 0;

  while ((move = getNextMove(node)) !== 0) {

    // prune?
    
    if ((wCount + bCount) > 6 && (move & MOVE_SPECIAL_MASK) === 0 && ev + 200 + MATERIAL[((move & MOVE_TOOBJ_MASK) >>> MOVE_TOOBJ_BITS) & PIECE_MASK] < alpha)
      continue;
    
    if (quickSee(turn, move) < 0)
      continue;
    

    makeMoveA(node, move);

    // legal?
    
    if (isKingAttacked(nextTurn) !== 0) {
    
      unmakeMove(node, move);
    
      uncacheA(node);
    
      continue;
    
    }
    

    numLegalMoves++;

    score = -qSearch(node.childNode, depth-1, nextTurn, -beta, -alpha);

    // unmake move

    unmakeMove(node, move);

    uncacheA(node);


    if (score > alpha) {
      if (score >= beta) {
        ttPut(TT_BETA, 0, beta, 0, node.ply, alpha, beta, ev);
        return score;
      }
      alpha = score;
    }
  }

  ttPut(TT_ALPHA, 0, alpha, 0, node.ply, alpha, beta, ev);

  return alpha;

}
function perft (node, depth, turn) {

  if (depth === 0)
    return 1;

  const nextTurn = turn ^ COLOR_MASK;
  const inCheck  = isKingAttacked(nextTurn);

  let totalNodes = 0;
  let move       = 0;

  node.inCheck = inCheck;

  node.rights = bdRights;
  node.ep     = bdEp;

  genMoves(node, turn);

  while ((move = getNextMove(node)) !== 0) {

    makeMoveA(node, move);

    // legal?
    
    if ((move & MOVE_LEGAL_MASK) === 0 && isKingAttacked(nextTurn) !== 0) {
    
      unmakeMove(node, move);
    
      bdRights = node.rights;
      bdEp     = node.ep;
    
      continue;
    
    }
    
    totalNodes += perft(node.childNode, depth-1, nextTurn);

    unmakeMove(node, move);
    
    bdRights = node.rights;
    bdEp     = node.ep;
    

  }

  return totalNodes;

}

function collectPV(node, move) {

  const cNode = node.childNode;
  const src   = cNode.pv;
  const dst   = node.pv;
  const len   = cNode.pvLen;

  for (let i = 0; i < len; i++)
    dst[i] = src[i];

  dst[len]   = move;
  node.pvLen = len + 1;

}

function report (score, depth, bound) {

  let pvStr = 'pv';
  for (let i=rootNode.pvLen-1; i >= 0; i--)
    pvStr += ' ' + formatMove(rootNode.pv[i]);

  const tim     = now() - statsStartTime;
  const nps     = (statsNodes * 1000) / tim | 0;
  const nodeStr = 'nodes ' + statsNodes + ' time ' + tim + ' nps ' + nps;

  const depthStr = 'depth ' + depth + ' seldepth ' + statsSelDepth;

  let scoreStr;
  if (Math.abs(score) > MINMATE) {
    let mateScore = (MATE - Math.abs(score)) / 2 | 0;
    if (score < 0)
      mateScore = -mateScore;
    scoreStr = 'score mate ' + mateScore;
  }
  else {
    scoreStr = 'score cp ' + score;
  }

  if (bound)
    scoreStr += ' ' + bound;

  const hashStr  = 'hashfull ' + (1000 * ttHashUsed / ttSize | 0);

  uciSend('info', depthStr, scoreStr, nodeStr, hashStr, pvStr);

}

function reportMultiPV (depth) {

  const tim     = now() - statsStartTime;
  const nps     = (statsNodes * 1000) / tim | 0;
  const nodeStr = 'nodes ' + statsNodes + ' time ' + tim + ' nps ' + nps;

  const depthStr = 'depth ' + depth + ' seldepth ' + statsSelDepth;
  const hashStr  = 'hashfull ' + (1000 * ttHashUsed / ttSize | 0);

  const n = Math.min(multiPV, multiPVMoves.length);

  for (let i = 0; i < n; i++) {

    const entry = multiPVMoves[i];

    let pvStr = 'pv';
    for (let j = entry.pvLen - 1; j >= 0; j--)
      pvStr += ' ' + formatMove(entry.pv[j]);

    let scoreStr;
    if (Math.abs(entry.score) > MINMATE) {
      let mateScore = (MATE - Math.abs(entry.score)) / 2 | 0;
      if (entry.score < 0)
        mateScore = -mateScore;
      scoreStr = 'score mate ' + mateScore;
    }
    else {
      scoreStr = 'score cp ' + entry.score;
    }

    uciSend('info', depthStr, 'multipv', (i + 1), scoreStr, nodeStr, hashStr, pvStr);
  }
}

//
// The weights are i16 in the .bin file so Int16Array storage is exact and
// halves the working set (and thus cache pressure) vs Int32Array.  The
// accumulators stay Int32Array; unclipped squared ReLU means their values
// have no hard i16 guarantee.
//

const net_h1_w_flat = new Int16Array(NET_I_SIZE * NET_H1_SIZE);  // us
const net_h2_w_flat = new Int16Array(NET_I_SIZE * NET_H1_SIZE);  // them
const net_h1_b      = new Int32Array(NET_H1_SIZE);
const net_o_w       = new Int16Array(NET_H1_SIZE*2);
let   net_o_b       = 0;

let ueFunc  = myround;
let ueKind  = 0;       // 1 = netMove, 2 = netCapture, 0 = the rest; see netEval()
let ueArgs0 = 0;
let ueArgs1 = 0;
let ueArgs2 = 0;
let ueArgs3 = 0;
let ueArgs4 = 0;
let ueArgs5 = 0;

// resolveAccs
//
// Each node owns the accumulators for its position but they are not
// written by makeMoveA(); it just flags the child dirty and leaves the
// pending update in ueFunc/ueArgs*.  The first thing that needs the
// accumulators calls this and the ue* func writes them from the parent's
// in one fused pass (node = parent + delta); there is no copy and
// nothing to restore on unmake.  Nodes that return before their resolve
// point (tt cutoffs, draws, hash eval stand pats etc) never pay for the
// update at all.  The globals are always valid here because a node
// resolves before making any moves of its own.
//

function resolveAccs (node) {

  if (node.accsDirty === 0)
    return;

  node.accsDirty = 0;

  ueFunc(node);

}

function netEval(node, turn) {

  const w = net_o_w;
  const N = NET_H1_SIZE | 0;

  let e = 0 | 0;

  if (node.accsDirty !== 0 && ueKind !== 0) {

    //
    // Fused resolve + eval for the two common cases (netMove/netCapture);
    // one pass writes the accumulators and does the output layer, instead
    // of streaming them twice.  relu(x)^2 === Math.imul(x,x) & ~(x >> 31),
    // bit-identical to the two-pass form.
    //

    node.accsDirty = 0;

    const map = IMAP;
    const h1w = net_h1_w_flat, h2w = net_h2_w_flat;

    const parent = node.parentNode;

    const s1 = parent.net_h1_a, s2 = parent.net_h2_a;
    const d1 = node.net_h1_a,   d2 = node.net_h2_a;

    let q1 = turn === WHITE ? 0 : N;  // us/them net_o_w offsets by pov
    let q2 = N - q1;

    let h = 0;

    if (ueKind === 1) {

      let p1 = map[(ueArgs0 << 8) + ueArgs1] | 0;
      let p2 = map[(ueArgs0 << 8) + ueArgs2] | 0;

      while (h < N) {

        const v1 = s1[h] + h1w[p2] - h1w[p1];
        const v2 = s2[h] + h2w[p2] - h2w[p1];

        d1[h] = v1;
        d2[h] = v2;

        e = (e + Math.imul(w[q1], Math.imul(v1, v1) & ~(v1 >> 31))
               + Math.imul(w[q2], Math.imul(v2, v2) & ~(v2 >> 31))) | 0;

        h++; p1++; p2++; q1++; q2++;

      }

    }

    else {

      let p1 = map[(ueArgs0 << 8) + ueArgs1] | 0;
      let p2 = map[(ueArgs2 << 8) + ueArgs3] | 0;
      let p3 = map[(ueArgs0 << 8) + ueArgs3] | 0;

      while (h < N) {

        const v1 = s1[h] + h1w[p3] - h1w[p2] - h1w[p1];
        const v2 = s2[h] + h2w[p3] - h2w[p2] - h2w[p1];

        d1[h] = v1;
        d2[h] = v2;

        e = (e + Math.imul(w[q1], Math.imul(v1, v1) & ~(v1 >> 31))
               + Math.imul(w[q2], Math.imul(v2, v2) & ~(v2 >> 31))) | 0;

        h++; p1++; p2++; p3++; q1++; q2++;

      }

    }

  }

  else {

    resolveAccs(node);

    const a  = node.net_a[turn >>> 3];
    const a1 = a[0];
    const a2 = a[1];

    let p1 = 0 | 0;
    let p2 = N | 0;

    while (p1 < N) {

      const x1 = a1[p1] | 0;
      const x2 = a2[p1] | 0;

      const y1 = (x1 + (x1 ^ (x1 >> 31)) - (x1 >> 31)) >> 1;
      const y2 = (x2 + (x2 ^ (x2 >> 31)) - (x2 >> 31)) >> 1;

      e = (e + Math.imul(w[p1], Math.imul(y1, y1)) + Math.imul(w[p2], Math.imul(y2, y2))) | 0;

      p1++; p2++;

    }

  }

  let e2 = e;

  e2 = (e2 / NET_QA) | 0;
  e2 = (e2 + (net_o_b | 0)) | 0;
  e2 = Math.imul(e2, NET_SCALE | 0) | 0;
  e2 = (e2 / NET_QAB) | 0;

  return e2 | 0;

}

function getWeightsBuffer() {

  //const b64 = WEIGHTS_B64.replace(/\s+/g, "");
  const b64 = WEIGHTS_B64;

  if (typeof Buffer !== 'undefined' && Buffer.from) {
    return Buffer.from(b64, 'base64');
  }

  const binStr = atob(b64);
  const n = binStr.length;
  const bytes = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    bytes[i] = binStr.charCodeAt(i);
  }

  return bytes;

}

function netLoad () {

  let offset = 0;

  const buffer = getWeightsBuffer();

  const dataView = new Int16Array(
    buffer.buffer,
    buffer.byteOffset,
    buffer.byteLength >> 1
  );

  for (let i = 0; i < NET_I_SIZE; i++) {
    const lozIndex = bullet2lozza(i);
    const h = i * NET_H1_SIZE;
    for (let j = 0; j < NET_H1_SIZE; j++) {
      net_h1_w_flat[lozIndex            * NET_H1_SIZE + j] = dataView[h + j];  // us
      net_h2_w_flat[flipIndex(lozIndex) * NET_H1_SIZE + j] = dataView[h + j];  // them
    }
  }

  offset += NET_I_SIZE * NET_H1_SIZE;
  for (let i = 0; i < NET_H1_SIZE; i++) {
    net_h1_b[i] = dataView[offset + i];
  }

  offset += NET_H1_SIZE;
  for (let i = 0; i < NET_H1_SIZE * 2; i++) {
    net_o_w[i] = dataView[offset + i];
  }

  offset += NET_H1_SIZE * 2;
  net_o_b = dataView[offset];

}

function netMove (node) {

  const frObj = ueArgs0 << 8;
  const from  = ueArgs1 | 0;
  const to    = ueArgs2 | 0;

  const map = IMAP;
  const h1w = net_h1_w_flat, h2w = net_h2_w_flat;

  const parent = node.parentNode;

  const s1 = parent.net_h1_a, s2 = parent.net_h2_a;
  const d1 = node.net_h1_a,   d2 = node.net_h2_a;

  const N = NET_H1_SIZE | 0;

  let h  = 0;
  let p1 = map[frObj + from] | 0;
  let p2 = map[frObj + to]   | 0;

  while (h < N) {
    d1[h] = s1[h] + h1w[p2] - h1w[p1];
    d2[h] = s2[h] + h2w[p2] - h2w[p1];
    h++; p1++; p2++;
  }

}

function netCapture (node) {

  const frObj = ueArgs0 << 8;
  const fr    = ueArgs1 | 0;
  const toObj = ueArgs2 << 8;
  const to    = ueArgs3 | 0;

  const map = IMAP;
  const h1w = net_h1_w_flat, h2w = net_h2_w_flat;

  const parent = node.parentNode;

  const s1 = parent.net_h1_a, s2 = parent.net_h2_a;
  const d1 = node.net_h1_a,   d2 = node.net_h2_a;

  const N = NET_H1_SIZE | 0;

  let h  = 0;
  let p1 = map[frObj + fr] | 0;
  let p2 = map[toObj + to] | 0;
  let p3 = map[frObj + to] | 0;

  while (h < N) {
    d1[h] = s1[h] + h1w[p3] - h1w[p2] - h1w[p1];
    d2[h] = s2[h] + h2w[p3] - h2w[p2] - h2w[p1];
    h++; p1++; p2++; p3++;
  }

}

function netPromote (node) {

  const pawnObj    = ueArgs0 << 8;
  const pawnFr     = ueArgs1 | 0;
  const pawnTo     = ueArgs2 | 0;
  const captureObj = ueArgs3 << 8;
  const promoteObj = ueArgs4 << 8;

  const map = IMAP;
  const h1w = net_h1_w_flat, h2w = net_h2_w_flat;

  const parent = node.parentNode;

  const s1 = parent.net_h1_a, s2 = parent.net_h2_a;
  const d1 = node.net_h1_a,   d2 = node.net_h2_a;

  const N = NET_H1_SIZE | 0;

  let h  = 0;
  let p1 = map[pawnObj    + pawnFr] | 0;
  let p2 = map[promoteObj + pawnTo] | 0;

  if (captureObj !== 0) {
    let p3 = map[captureObj + pawnTo] | 0;
    while (h < N) {
      d1[h] = s1[h] + h1w[p2] - h1w[p1] - h1w[p3];
      d2[h] = s2[h] + h2w[p2] - h2w[p1] - h2w[p3];
      h++; p1++; p2++; p3++;
    }
  }
  else {
    while (h < N) {
      d1[h] = s1[h] + h1w[p2] - h1w[p1];
      d2[h] = s2[h] + h2w[p2] - h2w[p1];
      h++; p1++; p2++;
    }
  }

}

function netEpCapture (node) {

  const pawnObj        = ueArgs0 << 8;
  const pawnFr         = ueArgs1 | 0;
  const pawnTo         = ueArgs2 | 0;
  const pawnCaptureObj = ueArgs3 << 8;
  const ep             = ueArgs4 | 0;

  const map = IMAP;
  const h1w = net_h1_w_flat, h2w = net_h2_w_flat;

  const parent = node.parentNode;

  const s1 = parent.net_h1_a, s2 = parent.net_h2_a;
  const d1 = node.net_h1_a,   d2 = node.net_h2_a;

  const N = NET_H1_SIZE | 0;

  let h  = 0;
  let p1 = map[pawnObj        + pawnFr] | 0;
  let p2 = map[pawnObj        + pawnTo] | 0;
  let p3 = map[pawnCaptureObj + ep] | 0;

  while (h < N) {
    d1[h] = s1[h] + h1w[p2] - h1w[p1] - h1w[p3];
    d2[h] = s2[h] + h2w[p2] - h2w[p1] - h2w[p3];
    h++; p1++; p2++; p3++;
  }

}

function netCastle (node) {

  const kingObj = ueArgs0 << 8;
  const kingFr  = ueArgs1 | 0;
  const kingTo  = ueArgs2 | 0;
  const rookObj = ueArgs3 << 8;
  const rookFr  = ueArgs4 | 0;
  const rookTo  = ueArgs5 | 0;

  const map = IMAP;
  const h1w = net_h1_w_flat, h2w = net_h2_w_flat;

  const parent = node.parentNode;

  const s1 = parent.net_h1_a, s2 = parent.net_h2_a;
  const d1 = node.net_h1_a,   d2 = node.net_h2_a;

  const N = NET_H1_SIZE | 0;

  let h  = 0;
  let p1 = map[kingObj + kingFr] | 0;
  let p2 = map[kingObj + kingTo] | 0;
  let p3 = map[rookObj + rookFr] | 0;
  let p4 = map[rookObj + rookTo] | 0;

  while (h < N) {
    d1[h] = s1[h] + h1w[p2] - h1w[p1] + h1w[p4] - h1w[p3];
    d2[h] = s2[h] + h2w[p2] - h2w[p1] + h2w[p4] - h2w[p3];
    h++; p1++; p2++; p3++; p4++;
  }

}

//
// Slow. Only use during init.
//

function flipIndex (index) {

  const piece         = Math.floor(index / 64);
  const square        = index % 64;
  const flippedSquare = square ^ 56;
  const flippedPiece  = (piece + 6) % 12;
  const flippedIndex  = flippedPiece * 64 + flippedSquare;

  return flippedIndex;

}

//
// bullet2lozza
//
// bullet index 0 is a1. Lozza index 0 is a8.
// The piece order is the same.
// Apply this when loading the weights from the bullet .bin file.
//
// Slow. Only use during init.
//

function bullet2lozza (index) {

  const piece        = Math.floor(index / 64);
  const bulletSquare = index % 64;
  const lozzaSquare  = bulletSquare ^ 56;          // map a1 to a8 etc
  const lozzaIndex   = piece * 64 + lozzaSquare;

  return lozzaIndex;

}

const bdB = new Uint8Array(144);    // pieces
const bdZ = new Uint8Array(144);    // pointers to w|bList

let bdTurn   = 0;
let bdRights = 0;
let bdEp     = 0;

const wList = new Uint8Array(16);
const bList = new Uint8Array(16);

const wbList = [wList, bList];

const wCounts = new Uint8Array(7);
const bCounts = new Uint8Array(7);

let wCount = 0;
let bCount = 0;

// zobrists

let twisterList  = new Uint32Array(624);
let twisterIndex = 0;

function twisterInit(seed) {

  const mt = twisterList;

  mt[0] = seed >>> 0;

  for (let i = 1; i < 624; i++) {
    mt[i] = (0x6C078965 * (mt[i - 1] ^ (mt[i - 1] >>> 30)) + i) >>> 0;
  }
}

function twisterFill() {

  const mt = twisterList;

  for (let i = 0; i < 624; i++) {
    let y = (mt[i] & 0x80000000) + (mt[(i + 1) % 624] & 0x7FFFFFFF);
    mt[i] = mt[(i + 397) % 624] ^ (y >>> 1);
    if (y % 2 !== 0) {
      mt[i] ^= 0x9908B0DF;
    }
  }
}

function twisterRand() {

  const mt = twisterList;

  if (twisterIndex === 0)
    twisterFill();

  let y = mt[twisterIndex];
  y ^= y >>> 11;
  y ^= (y << 7)  & 0x9D2C5680;
  y ^= (y << 15) & 0xEFC60000;
  y ^= y >>> 18;

  twisterIndex = (twisterIndex + 1) % 624;

  return y >>> 0;
}

twisterInit(0x9E3779B9);

let loTurn = twisterRand();
let hiTurn = twisterRand();

const loObjPieces = new Int32Array(15 * 256);
const hiObjPieces = new Int32Array(15 * 256);

for (let i=0; i < 15 * 256; i++) {
  loObjPieces[i] = twisterRand();
  hiObjPieces[i] = twisterRand();
}

const loRights = new Int32Array(16);
const hiRights = new Int32Array(16);

for (let i=0; i < 16; i++) {
  loRights[i] = twisterRand();
  hiRights[i] = twisterRand();
}

const loEP = new Int32Array(144);
const hiEP = new Int32Array(144);

for (let i=0; i < 144; i++) {
  loEP[i] = twisterRand();
  hiEP[i] = twisterRand();
}

// hash

let loHash = 0;
let hiHash = 0;

let repLo = 0;
let repHi = 0;

const repLoHash = new Int32Array(1024);
const repHiHash = new Int32Array(1024);

function makeMoveA (node, move) {

  const b = bdB;
  const z = bdZ;

  const fr      = (move & MOVE_FR_MASK   ) >>> MOVE_FR_BITS;
  const to      = (move & MOVE_TO_MASK   ) >>> MOVE_TO_BITS;
  const toObj   = (move & MOVE_TOOBJ_MASK) >>> MOVE_TOOBJ_BITS;
  const frObj   = (move & MOVE_FROBJ_MASK) >>> MOVE_FROBJ_BITS;
  const frPiece = frObj & PIECE_MASK;
  const frCol   = frObj & COLOR_MASK;

  // slide piece
  
  b[fr] = 0;
  b[to] = frObj;
  
  node.frZ = z[fr];
  node.toZ = z[to];

  z[fr] = NO_Z;
  z[to] = node.frZ;

  node.childNode.accsDirty = 1;  // resolved lazily; see resolveAccs() in net.js
  
  loHash ^= loObjPieces[(frObj << 8) + fr];
  hiHash ^= hiObjPieces[(frObj << 8) + fr];
  
  loHash ^= loObjPieces[(frObj << 8) + to];
  hiHash ^= hiObjPieces[(frObj << 8) + to];
  
  wbList[frCol >>> 3][node.frZ] = to;
  
  // clear rights?
  
  if (bdRights !== 0) {
  
    loHash ^= loRights[bdRights];
    hiHash ^= hiRights[bdRights];
  
    bdRights &= MASK_RIGHTS[fr] & MASK_RIGHTS[to];
  
    loHash ^= loRights[bdRights];
    hiHash ^= hiRights[bdRights];
  
  }
  
  // capture?
  
  if (toObj !== 0) {
  
    const toPiece = toObj & PIECE_MASK;
    const toCol   = toObj & COLOR_MASK;
  
    loHash ^= loObjPieces[(toObj << 8) + to];
    hiHash ^= hiObjPieces[(toObj << 8) + to];
  
    if (toCol === WHITE) {
  
      wList[node.toZ] = EMPTY;
  
      wCounts[toPiece]--;
      wCount--;
    }
  
    else {
  
      bList[node.toZ] = EMPTY;
  
      bCounts[toPiece]--;
      bCount--;
    }
  
    ueFunc  = netCapture;
    ueKind  = 2;
    ueArgs0 = frObj;
    ueArgs1 = fr;
    ueArgs2 = toObj;
    ueArgs3 = to;
  
  }
  
  else {
  
    ueFunc  = netMove;
    ueKind  = 1;
    ueArgs0 = frObj;
    ueArgs1 = fr;
    ueArgs2 = to;
  
  }
  
  // reset EP
  
  loHash ^= loEP[bdEp];
  hiHash ^= hiEP[bdEp];
  
  bdEp = 0;
  
  loHash ^= loEP[bdEp];
  hiHash ^= hiEP[bdEp];
  

  if ((move & MOVE_SPECIAL_MASK) !== 0) {
    // ikky stuff
    
    if (frCol === WHITE) {
    
      const ep = to + 12;
    
      if ((move & MOVE_EPMAKE_MASK) !== 0) {
    
        ueFunc  = netMove;
        ueKind  = 1;
        ueArgs0 = frObj;
        ueArgs1 = fr;
        ueArgs2 = to;
    
        loHash ^= loEP[bdEp];
        hiHash ^= hiEP[bdEp];
    
        bdEp = ep;
    
        loHash ^= loEP[bdEp];
        hiHash ^= hiEP[bdEp];
      }
    
      else if ((move & MOVE_EPTAKE_MASK) !== 0) {
    
        ueFunc  = netEpCapture;
        ueKind  = 0;
        ueArgs0 = frObj;
        ueArgs1 = fr;
        ueArgs2 = to;
        ueArgs3 = B_PAWN;
        ueArgs4 = ep;
    
        b[ep]    = 0;
        node.epZ = z[ep];
        z[ep]    = NO_Z;
    
        bList[node.epZ] = EMPTY;
    
        loHash ^= loObjPieces[(B_PAWN << 8) + ep];
        hiHash ^= hiObjPieces[(B_PAWN << 8) + ep];
    
        bCounts[PAWN]--;
        bCount--;
      }
    
      else if ((move & MOVE_PROMOTE_MASK) !== 0) {
    
        const pro = ((move & MOVE_PROMAS_MASK) >>> MOVE_PROMAS_BITS) + 2;  //NBRQ
        b[to]     = WHITE | pro;
    
        ueFunc  = netPromote;
        ueKind  = 0;
        ueArgs0 = W_PAWN;
        ueArgs1 = fr;
        ueArgs2 = to;
        ueArgs3 = toObj;
        ueArgs4 = pro|WHITE;
    
        loHash ^= loObjPieces[(W_PAWN << 8) + to];
        hiHash ^= hiObjPieces[(W_PAWN << 8) + to];
        loHash ^= loObjPieces[((WHITE|pro) << 8) + to];
        hiHash ^= hiObjPieces[((WHITE|pro) << 8) + to];
    
        wCounts[PAWN]--;
        wCounts[pro]++;
    
      }
    
      else if (move === MOVE_E1G1) {
    
        ueFunc  = netCastle;
        ueKind  = 0;
        ueArgs0 = W_KING;
        ueArgs1 = fr;
        ueArgs2 = to;
        ueArgs3 = W_ROOK;
        ueArgs4 = H1;
        ueArgs5 = F1;
    
        b[H1] = 0;
        b[F1] = W_ROOK;
        z[F1] = z[H1];
        z[H1] = NO_Z;
    
        wList[z[F1]] = F1;
    
        loHash ^= loObjPieces[(W_ROOK << 8) + H1];
        hiHash ^= hiObjPieces[(W_ROOK << 8) + H1];
        loHash ^= loObjPieces[(W_ROOK << 8) + F1];
        hiHash ^= hiObjPieces[(W_ROOK << 8) + F1];
    
      }
    
      else if (move === MOVE_E1C1) {
    
        ueFunc  = netCastle;
        ueKind  = 0;
        ueArgs0 = W_KING;
        ueArgs1 = fr;
        ueArgs2 = to;
        ueArgs3 = W_ROOK;
        ueArgs4 = A1;
        ueArgs5 = D1;
    
        b[A1] = 0;
        b[D1] = W_ROOK;
        z[D1] = z[A1];
        z[A1] = NO_Z;
    
        wList[z[D1]] = D1;
    
        loHash ^= loObjPieces[(W_ROOK << 8) + A1];
        hiHash ^= hiObjPieces[(W_ROOK << 8) + A1];
        loHash ^= loObjPieces[(W_ROOK << 8) + D1];
        hiHash ^= hiObjPieces[(W_ROOK << 8) + D1];
    
      }
    }
    
    else {
    
      const ep = to - 12;
    
      if ((move & MOVE_EPMAKE_MASK) !== 0) {
    
        ueFunc  = netMove;
        ueKind  = 1;
        ueArgs0 = frObj;
        ueArgs1 = fr;
        ueArgs2 = to;
    
        loHash ^= loEP[bdEp];
        hiHash ^= hiEP[bdEp];
    
        bdEp = ep;
    
        loHash ^= loEP[bdEp];
        hiHash ^= hiEP[bdEp];
      }
    
      else if ((move & MOVE_EPTAKE_MASK) !== 0) {
    
        ueFunc  = netEpCapture;
        ueKind  = 0;
        ueArgs0 = frObj;
        ueArgs1 = fr;
        ueArgs2 = to;
        ueArgs3 = W_PAWN;
        ueArgs4 = ep;
    
        b[ep]    = 0;
        node.epZ = z[ep];
        z[ep]    = NO_Z;
    
        wList[node.epZ] = EMPTY;
    
        loHash ^= loObjPieces[(W_PAWN << 8) + ep];
        hiHash ^= hiObjPieces[(W_PAWN << 8) + ep];
    
        wCounts[PAWN]--;
        wCount--;
      }
    
      else if ((move & MOVE_PROMOTE_MASK) !== 0) {
    
        const pro = ((move & MOVE_PROMAS_MASK) >>> MOVE_PROMAS_BITS) + 2;  //NBRQ
        b[to]     = BLACK | pro;
    
        ueFunc  = netPromote;
        ueKind  = 0;
        ueArgs0 = B_PAWN;
        ueArgs1 = fr;
        ueArgs2 = to;
        ueArgs3 = toObj;
        ueArgs4 = pro|BLACK;
    
        loHash ^= loObjPieces[(B_PAWN << 8) + to];
        hiHash ^= hiObjPieces[(B_PAWN << 8) + to];
        loHash ^= loObjPieces[((pro|BLACK) << 8) + to];
        hiHash ^= hiObjPieces[((pro|BLACK) << 8) + to];
    
        bCounts[PAWN]--;
        bCounts[pro]++;
    
      }
    
      else if (move === MOVE_E8G8) {
    
        ueFunc  = netCastle;
        ueKind  = 0;
        ueArgs0 = B_KING;
        ueArgs1 = fr;
        ueArgs2 = to;
        ueArgs3 = B_ROOK;
        ueArgs4 = H8;
        ueArgs5 = F8;
    
        b[H8] = 0;
        b[F8] = B_ROOK;
        z[F8] = z[H8];
        z[H8] = NO_Z;
    
        bList[z[F8]] = F8;
    
        loHash ^= loObjPieces[(B_ROOK << 8) + H8];
        hiHash ^= hiObjPieces[(B_ROOK << 8) + H8];
        loHash ^= loObjPieces[(B_ROOK << 8) + F8];
        hiHash ^= hiObjPieces[(B_ROOK << 8) + F8];
    
      }
    
      else if (move === MOVE_E8C8) {
    
        ueFunc  = netCastle;
        ueKind  = 0;
        ueArgs0 = B_KING;
        ueArgs1 = fr;
        ueArgs2 = to;
        ueArgs3 = B_ROOK;
        ueArgs4 = A8;
        ueArgs5 = D8;
    
        b[A8] = 0;
        b[D8] = B_ROOK;
        z[D8] = z[A8];
        z[A8] = NO_Z;
    
        bList[z[D8]] = D8;
    
        loHash ^= loObjPieces[(B_ROOK << 8) + A8];
        hiHash ^= hiObjPieces[(B_ROOK << 8) + A8];
        loHash ^= loObjPieces[(B_ROOK << 8) + D8];
        hiHash ^= hiObjPieces[(B_ROOK << 8) + D8];
    
      }
    
    }
    
  }

  // flip turn in hash
  
  loHash ^= loTurn;
  hiHash ^= hiTurn;
  
  // push rep hash
  //
  // Repetitions are cancelled by pawn moves, castling, captures, EP
  // and promotions; i.e. moves that are not reversible.  The nearest
  // repetition is 5 indexes back from the current one and then that
  // and every other one entry is a possible rep.  Can also check for
  // 50 move rule by testing hi-lo >= 100 - it's not perfect because of
  // the pawn move reset but it's a type 2 error, so safe.
  //
  
  repLoHash[repHi] = loHash;
  repHiHash[repHi] = hiHash;
  
  repHi++;
  
  if ((move & (MOVE_SPECIAL_MASK | MOVE_TOOBJ_MASK)) || frPiece === PAWN)
    repLo = repHi;
  

}

// unmakeMove

function unmakeMove (node, move) {

  const b = bdB;
  const z = bdZ;

  const fr    = (move & MOVE_FR_MASK)    >>> MOVE_FR_BITS;
  const to    = (move & MOVE_TO_MASK)    >>> MOVE_TO_BITS;
  const toObj = (move & MOVE_TOOBJ_MASK) >>> MOVE_TOOBJ_BITS;
  const frObj = (move & MOVE_FROBJ_MASK) >>> MOVE_FROBJ_BITS;
  const frCol = frObj & COLOR_MASK;

  b[fr] = frObj;
  b[to] = toObj;

  z[fr] = node.frZ;
  z[to] = node.toZ;

  wbList[frCol >>> 3][node.frZ] = fr;

  // capture?
  
  if (toObj !== 0) {
  
    const toPiece = toObj & PIECE_MASK;
    const toCol   = toObj & COLOR_MASK;
  
    if (toCol === WHITE) {
  
      wList[node.toZ] = to;
  
      wCounts[toPiece]++;
      wCount++;
    }
  
    else {
  
      bList[node.toZ] = to;
  
      bCounts[toPiece]++;
      bCount++;
    }
  
  }
  

  if ((move & MOVE_SPECIAL_MASK) !== 0) {
    // ikky stuff
    
    if ((frObj & COLOR_MASK) === WHITE) {
    
      const ep = to + 12;
    
      if (move & MOVE_EPTAKE_MASK) {
    
        b[ep] = B_PAWN;
        z[ep] = node.epZ;
    
        bList[node.epZ] = ep;
    
        bCounts[PAWN]++;
        bCount++;
    
      }
    
      else if (move & MOVE_PROMOTE_MASK) {
    
        const pro = ((move & MOVE_PROMAS_MASK) >>> MOVE_PROMAS_BITS) + 2;  //NBRQ
    
        wCounts[PAWN]++;
        wCounts[pro]--;
    
      }
    
      else if (move === MOVE_E1G1) {
    
        b[H1] = W_ROOK;
        b[F1] = 0;
        z[H1] = z[F1];
        z[F1] = NO_Z;
    
        wList[z[H1]] = H1;
      }
    
      else if (move === MOVE_E1C1) {
    
        b[A1] = W_ROOK;
        b[D1] = 0;
        z[A1] = z[D1];
        z[D1] = NO_Z;
    
        wList[z[A1]] = A1;
      }
    }
    
    else {
    
      const ep = to - 12;
    
      if (move & MOVE_EPTAKE_MASK) {
    
        b[ep] = W_PAWN;
        z[ep] = node.epZ;
    
        wList[node.epZ] = ep;
    
        wCounts[PAWN]++;
        wCount++;
      }
    
      else if (move & MOVE_PROMOTE_MASK) {
    
        const pro = ((move & MOVE_PROMAS_MASK) >>> MOVE_PROMAS_BITS) + 2;  //NBRQ
    
        bCounts[PAWN]++;
        bCounts[pro]--;
    
      }
    
      else if (move === MOVE_E8G8) {
    
        b[H8] = B_ROOK;
        b[F8] = 0;
        z[H8] = z[F8];
        z[F8] = NO_Z;
    
        bList[z[H8]] = H8;
      }
    
      else if (move === MOVE_E8C8) {
    
        b[A8] = B_ROOK;
        b[D8] = 0;
        z[A8] = z[D8];
        z[D8] = NO_Z;
    
        bList[z[A8]] = A8;
      }
    }
    
  }

}

let ttDefault = 16;  // mb
let ttSize    = 1;
let ttMask    = 0;

let ttLo    = new Int32Array(ttSize);
let ttHi    = new Int32Array(ttSize);
let ttType  = new Uint8Array(ttSize);
let ttDepth = new Int8Array(ttSize);
let ttMove  = new Uint32Array(ttSize);
let ttEval  = new Int16Array(ttSize);
let ttScore = new Int16Array(ttSize);
// ===
const ttWidth = 18;
// ===

let ttHashUsed = 0;

function ttResize(N_MB) {

  const bytesPerEntry  = ttWidth;
  const requestedBytes = N_MB * 1024 * 1024;
  const entriesNeeded  = requestedBytes / bytesPerEntry;
  const pow2           = Math.ceil(Math.log2(entriesNeeded));

  ttSize = 1 << pow2;
  ttMask = ttSize - 1;

  ttLo    = new Int32Array(ttSize);
  ttHi    = new Int32Array(ttSize);
  ttType  = new Uint8Array(ttSize);
  ttDepth = new Int8Array(ttSize);
  ttMove  = new Uint32Array(ttSize);
  ttEval  = new Int16Array(ttSize);
  ttScore = new Int16Array(ttSize);

  const sm   = silentMode;
  silentMode = 0;

  uciSend('info tt bits', pow2, 'entries', ttSize, '(0x' + ttSize.toString(16) + ')', 'mb', ttWidth * ttSize);

  silentMode = sm;

}

function ttPut (type, depth, score, move, ply, alpha, beta, ev) {

  const idx = loHash & ttMask;

  if (depth === 0 && ttType[idx] !== TT_EMPTY && ttDepth[idx] > 0)
    return;

  if (ttType[idx] === TT_EMPTY)
    ttHashUsed++;

  if (score <= -MINMATE && score >= -MATE)
    score -= ply;

  else if (score >= MINMATE && score <= MATE)
    score += ply;

  ttLo[idx]    = loHash;
  ttHi[idx]    = hiHash;
  ttType[idx]  = type;
  ttDepth[idx] = depth;
  ttScore[idx] = score;
  ttEval[idx]  = ev;

  if (move !== 0)
    ttMove[idx] = move & MOVE_CLEAN_MASK;

}

// ttGet

function ttGet (node, depth, alpha, beta) {

  const idx   = loHash & ttMask;
  const type  = ttType[idx];

  node.hashMove = 0;
  node.hashEval = INF;

  if (type === TT_EMPTY)
    return TTSCORE_UNKNOWN;

  const lo = ttLo[idx];
  const hi = ttHi[idx];

  if (lo !== loHash || hi !== hiHash)
    return TTSCORE_UNKNOWN;

  //
  // Set the hash move before the depth check
  // so that iterative deepening works.
  //

  if (ttValidate(ttMove[idx]) !== 0)
    node.hashMove = ttMove[idx];

  node.hashEval = ttEval[idx];

  if (ttDepth[idx] < depth)
    return TTSCORE_UNKNOWN;

  var score = ttScore[idx];

  if (score <= -MINMATE && score >= -MATE)
    score += node.ply;

  else if (score >= MINMATE && score <= MATE)
    score -= node.ply;

  if (type === TT_EXACT)
    return score;

  if (type === TT_ALPHA && score <= alpha)
    return score;

  if (type === TT_BETA && score >= beta)
    return score;

  return TTSCORE_UNKNOWN;

}

// ttUpdateEval

function ttUpdateEval (ev) {

  const idx = loHash & ttMask;

  if (ttType[idx] !== TT_EMPTY && ttLo[idx] === loHash && ttHi[idx] === hiHash)
    ttEval[idx] = ev;

}

// ttInit

function ttInit () {

  ttType.fill(TT_EMPTY);
  ttMove.fill(0);

  ttHashUsed = 0;

}

// ttValidate

function ttValidate (move) {

  const b = bdB;

  const fr    = (move & MOVE_FR_MASK   ) >>> MOVE_FR_BITS;
  const frObj = (move & MOVE_FROBJ_MASK) >>> MOVE_FROBJ_BITS;

  if (b[fr] !== frObj)
    return 0;

  const to    = (move & MOVE_TO_MASK   ) >>> MOVE_TO_BITS;
  const toObj = (move & MOVE_TOOBJ_MASK) >>> MOVE_TOOBJ_BITS;

  if (b[to] !== toObj)
    return 0;

  return 1;

}

function position (bd, turn, rights, ep, moves) {

  for (let i=0; i < nodes.length; i++)
    initNode(nodes[i]);

  loHash = 0;
  hiHash = 0;

  // turn
  
  if (turn == 'w')
    bdTurn = WHITE;
  
  else {
    bdTurn = BLACK;
    loHash ^= loTurn;
    hiHash ^= hiTurn;
  }
  
  // rights
  
  bdRights = 0;
  
  for (let i=0; i < rights.length; i++) {
  
    var ch = rights.charAt(i);
  
    if (ch == 'K') bdRights |= WHITE_RIGHTS_KING;
    if (ch == 'Q') bdRights |= WHITE_RIGHTS_QUEEN;
    if (ch == 'k') bdRights |= BLACK_RIGHTS_KING;
    if (ch == 'q') bdRights |= BLACK_RIGHTS_QUEEN;
  }
  
  loHash ^= loRights[bdRights];
  hiHash ^= hiRights[bdRights];
  
  // board
  
  bdB.fill(EDGE);
  
  for (var i=0; i < B88.length; i++)
    bdB[B88[i]] = 0;
  
  bdZ.fill(NO_Z);
  
  wCounts.fill(0);
  bCounts.fill(0);
  
  wList.fill(EMPTY);
  bList.fill(EMPTY);
  
  wCount = 1;
  bCount = 1;
  
  let sq = 0;
  
  for (let j=0; j < bd.length; j++) {
  
    const ch  = bd.charAt(j);
    const chn = parseInt(ch);
  
    while (bdB[sq] === EDGE)
      sq++;
  
    if (isNaN(chn)) {
  
      if (ch != '/') {
  
        const obj   = MAP[ch];
        const piece = obj & PIECE_MASK;
        const col   = obj & COLOR_MASK;
  
        bdB[sq] = obj;
  
        if (col === WHITE) {
          if (piece === KING) {
            wList[0] = sq;
            bdZ[sq] = 0;
            wCounts[KING]++;
          }
          else {
            wList[wCount] = sq;
            bdZ[sq] = wCount;
            wCounts[piece]++;
            wCount++;
          }
        }
  
        else {
          if (piece === KING) {
            bList[0] = sq;
            bdZ[sq] = 0;
            bCounts[KING]++;
          }
          else {
            bList[bCount] = sq;
            bdZ[sq] = bCount;
            bCounts[piece]++;
            bCount++;
          }
        }
  
        loHash ^= loObjPieces[(obj << 8) + sq];
        hiHash ^= hiObjPieces[(obj << 8) + sq];
  
        sq++;
      }
    }
  
    else {
  
      for (let k=0; k < chn; k++) {
        bdB[sq] = 0;
        sq++;
      }
    }
  
  }
  
  // ep
  
  if (ep.length === 2)
    bdEp = COORDS.indexOf(ep)
  else
    bdEp = 0;
  
  loHash ^= loEP[bdEp];
  hiHash ^= hiEP[bdEp];
  

  repLo = 0;
  repHi = 0;

  for (let i=0; i < moves.length; i++) {
    // play move
    
    const moveStr = moves[i];
    
    let move = 0;
    
    genMoves(rootNode, bdTurn);
    
    while ((move = getNextMove(rootNode)) !== 0) {
    
      const moveStr2 = formatMove(move);
    
      if (moveStr == moveStr2) {
        makeMoveA(rootNode, move);
        bdTurn ^= COLOR_MASK;
        break;
      }
    
    }
    
  }

  // compact
  
  const wList2 = new Uint8Array(16);
  const bList2 = new Uint8Array(16);
  
  let next = 0;
  
  for (let i=0; i < 16; i++) {
    const sq = wList[i];
    if (sq) {
      bdZ[sq] = next;
      wList2[next++] = sq;
    }
  }
  
  wList.set(wList2);
  
  next = 0;
  
  for (let i=0; i < 16; i++) {
    const sq = bList[i];
    if (sq) {
      bdZ[sq] = next;
      bList2[next++] = sq;
    }
  }
  
  bList.set(bList2);
  
  // ue

  const a1 = rootNode.net_h1_a;
  const a2 = rootNode.net_h2_a;

  a1.set(net_h1_b);
  a2.set(net_h1_b);

  for (let sq=0; sq < 64; sq++) {

    const fr    = B88[sq];
    const frObj = bdB[fr];

    if (frObj === 0)
      continue;

    const off1 = IMAP[(frObj << 8) + fr];

    for (let h=0; h < NET_H1_SIZE; h++) {
      const idx1 = off1 + h;
      a1[h] += net_h1_w_flat[idx1];
      a2[h] += net_h2_w_flat[idx1];
    }

  }
  
  initNode(rootNode);
  objHistory.fill(BASE_HISSLIDE);

}

function genMoves (node, turn) {

  node.stage     = 0;
  node.numMoves  = 0;
  node.numMoves2 = 0;
  node.next      = 0;

  const b = bdB;
  const inCheck = node.inCheck;

  // colour based stuff
  
  if (turn === WHITE) {
  
    var offsetOrth  = -12;
    var offsetDiag1 = -13;
    var offsetDiag2 = -11;
    var homeRank    = 2;
    var promoteRank = 7;
    var rights      = bdRights & WHITE_RIGHTS;
    var pList       = wList;
    var theirKingSq = bList[0];
    var pCount      = wCount;
    var CAPTURE     = IS_BNK;
    var aligned     = ALIGNED[wList[0]];
  
    if (inCheck === 0 && rights !== 0) {
  
      if (((rights & WHITE_RIGHTS_KING) !== 0) && b[F1] === 0 && b[G1] === 0 && b[SQG2] !== B_KING && b[SQH2] !== B_KING && isAttacked(F1,BLACK) === 0)
        addCastle(node, MOVE_E1G1);
  
      if (((rights & WHITE_RIGHTS_QUEEN) !== 0) && b[B1] === 0 && b[C1] === 0 && b[D1] === 0 && b[SQB2] !== B_KING && b[SQC2] !== B_KING && isAttacked(D1,BLACK) === 0)
        addCastle(node, MOVE_E1C1);
    }
  
  }
  
  else {
  
    var offsetOrth  = 12;
    var offsetDiag1 = 13;
    var offsetDiag2 = 11;
    var homeRank    = 7;
    var promoteRank = 2;
    var rights      = bdRights & BLACK_RIGHTS;
    var pList       = bList;
    var theirKingSq = wList[0];
    var pCount      = bCount;
    var CAPTURE     = IS_WNK;
    var aligned     = ALIGNED[bList[0]];
  
    if (inCheck === 0 && rights !== 0) {
  
      if (((rights & BLACK_RIGHTS_KING) !== 0) && b[F8] === 0 && b[G8] === 0 && b[SQG7] !== B_KING && b[SQH7] !== B_KING && isAttacked(F8,WHITE) === 0)
        addCastle(node, MOVE_E8G8);
  
      if (((rights & BLACK_RIGHTS_QUEEN) !== 0) && b[B8] === 0 && b[C8] === 0 && b[D8] === 0 && b[SQB7] !== B_KING && b[SQC7] !== B_KING && isAttacked(D8,WHITE) === 0)
        addCastle(node, MOVE_E8C8);
    }
  
  }
  

  let next   = 0;
  let count  = 0;
  let to     = 0;
  let toObj  = 0;
  let myMove = 0;

  while (count < pCount) {

    const fr = pList[next];
    if (fr === 0) {
      next++;
      continue;
    }

    const frObj     = b[fr];
    const frPiece   = frObj & PIECE_MASK;
    const frMove    = (frObj << MOVE_FROBJ_BITS) | (fr << MOVE_FR_BITS);
    const frRank    = RANK[fr];
    const legalMask = inCheck === 0 && aligned[fr] === 0 ? MOVE_LEGAL_MASK : 0;

    switch (frPiece) {
      case 1: {
        // P
        
        // orth
        
        to    = fr + offsetOrth;
        toObj = b[to];
        
        if (toObj === 0) {
        
          if (frRank === promoteRank)
            addPromotion(node, frMove | to | legalMask);
        
          else {
            addSlide(node, frMove | to | legalMask);
        
            if (frRank === homeRank) {
        
              to += offsetOrth;
              if (b[to] === 0)
                addSlide(node, frMove | to | MOVE_EPMAKE_MASK | legalMask);
            }
          }
        
        }
        
        // diag1
        
        to    = fr + offsetDiag1;
        toObj = b[to];
        
        if (CAPTURE[toObj] !== 0) {
        
          if (frRank === promoteRank)
            addPromotion(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to | legalMask);
          else
            addCapture(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to | legalMask);
        }
        
        else if (toObj === 0 && to === bdEp)
          addEPTake(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to | MOVE_EPTAKE_MASK);
        
        // diag2
        
        to    = fr + offsetDiag2;
        toObj = b[to];
        
        if (CAPTURE[toObj] !== 0) {
        
          if (frRank === promoteRank)
            addPromotion(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to | legalMask);
          else
            addCapture(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to | legalMask);
        }
        
        else if (toObj === 0 && to === bdEp)
          addEPTake(node, frMove | to | MOVE_EPTAKE_MASK);
        
        
        break;
        
      }
      case 2: {
        // N
        
        myMove = frMove | legalMask;
        
        to = fr + 25;
        if ((toObj = b[to]) === 0)
          addSlide(node, myMove | to);
        else if (CAPTURE[toObj] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 25;
        if ((toObj = b[to]) === 0)
          addSlide(node, myMove | to);
        else if (CAPTURE[toObj] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 23;
        if ((toObj = b[to]) === 0)
          addSlide(node, myMove | to);
        else if (CAPTURE[toObj] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 23;
        if ((toObj = b[to]) === 0)
          addSlide(node, myMove | to);
        else if (CAPTURE[toObj] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 14;
        if ((toObj = b[to]) === 0)
          addSlide(node, myMove | to);
        else if (CAPTURE[toObj] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 14;
        if ((toObj = b[to]) === 0)
          addSlide(node, myMove | to);
        else if (CAPTURE[toObj] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 10;
        if ((toObj = b[to]) === 0)
          addSlide(node, myMove | to);
        else if (CAPTURE[toObj] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 10;
        if ((toObj = b[to]) === 0)
          addSlide(node, myMove | to);
        else if (CAPTURE[toObj] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        break;
        
      }
      case 3: {
        // B
        
        myMove = frMove | legalMask;
        
        to = fr + 11;
        while (b[to] === 0)
          addSlide(node, myMove | to), to += 11;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 11;
        while (b[to] === 0)
          addSlide(node, myMove | to), to -= 11;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 13;
        while (b[to] === 0)
          addSlide(node, myMove | to), to += 13;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 13;
        while (b[to] === 0)
          addSlide(node, myMove | to), to -= 13;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        break;
        
      }
      case 4: {
        // R
        
        myMove = frMove | legalMask;
        
        to = fr + 1;
        while (b[to] === 0)
          addSlide(node, myMove | to), to += 1;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 1;
        while (b[to] === 0)
          addSlide(node, myMove | to), to -= 1;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 12;
        while (b[to] === 0)
          addSlide(node, myMove | to), to += 12;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 12;
        while (b[to] === 0)
          addSlide(node, myMove | to), to -= 12;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        break;
        
      }
      case 5: {
        // B
        
        myMove = frMove | legalMask;
        
        to = fr + 11;
        while (b[to] === 0)
          addSlide(node, myMove | to), to += 11;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 11;
        while (b[to] === 0)
          addSlide(node, myMove | to), to -= 11;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 13;
        while (b[to] === 0)
          addSlide(node, myMove | to), to += 13;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 13;
        while (b[to] === 0)
          addSlide(node, myMove | to), to -= 13;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        // R
        
        myMove = frMove | legalMask;
        
        to = fr + 1;
        while (b[to] === 0)
          addSlide(node, myMove | to), to += 1;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 1;
        while (b[to] === 0)
          addSlide(node, myMove | to), to -= 1;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 12;
        while (b[to] === 0)
          addSlide(node, myMove | to), to += 12;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 12;
        while (b[to] === 0)
          addSlide(node, myMove | to), to -= 12;
        if (CAPTURE[toObj = b[to]] !== 0)
          addCapture(node, myMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        break;
        
      }
      case 6: {
        // K
        
        to = fr + 11;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0) {
          if ((toObj = b[to]) === 0)
            addSlide(node, frMove | to);
          else if (CAPTURE[toObj] !== 0)
            addCapture(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        }
        
        to = fr - 11;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0) {
          if ((toObj = b[to]) === 0)
            addSlide(node, frMove | to);
          else if (CAPTURE[toObj] !== 0)
            addCapture(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        }
        
        to = fr + 13;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0) {
          if ((toObj = b[to]) === 0)
            addSlide(node, frMove | to);
          else if (CAPTURE[toObj] !== 0)
            addCapture(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        }
        
        to = fr - 13;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0) {
          if ((toObj = b[to]) === 0)
            addSlide(node, frMove | to);
          else if (CAPTURE[toObj] !== 0)
            addCapture(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        }
        
        to = fr + 1;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0) {
          if ((toObj = b[to]) === 0)
            addSlide(node, frMove | to);
          else if (CAPTURE[toObj] !== 0)
            addCapture(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        }
        
        to = fr - 1;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0) {
          if ((toObj = b[to]) === 0)
            addSlide(node, frMove | to);
          else if (CAPTURE[toObj] !== 0)
            addCapture(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        }
        
        to = fr + 12;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0) {
          if ((toObj = b[to]) === 0)
            addSlide(node, frMove | to);
          else if (CAPTURE[toObj] !== 0)
            addCapture(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        }
        
        to = fr - 12;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0) {
          if ((toObj = b[to]) === 0)
            addSlide(node, frMove | to);
          else if (CAPTURE[toObj] !== 0)
            addCapture(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        }
        
        break;
        
      }
    }

    next++;
    count++

  }
}

// genQMoves

function genQMoves (node, turn) {

  node.stage     = 0;
  node.numMoves  = 0;
  node.numMoves2 = 0;
  node.next      = 0;

  const b = bdB;

  // colour based stuff
  
  if (turn === WHITE) {
  
    var offsetOrth  = -12;
    var offsetDiag1 = -13;
    var offsetDiag2 = -11;
    var promoteRank = 7;
    var pList       = wList;
    var theirKingSq = bList[0];
    var pCount      = wCount;
    var CAPTURE     = IS_BNK;
  
  }
  
  else {
  
    var offsetOrth  = 12;
    var offsetDiag1 = 13;
    var offsetDiag2 = 11;
    var promoteRank = 2;
    var pList       = bList;
    var theirKingSq = wList[0];
    var pCount      = bCount;
    var CAPTURE     = IS_WNK;
  
  }
  

  let next  = 0;
  let count = 0;
  let to    = 0;
  let toObj = 0;

  while (count < pCount) {

    const fr = pList[next];
    if (fr === 0) {
      next++;
      continue;
    }

    const frObj   = b[fr];
    const frPiece = frObj & PIECE_MASK;
    const frMove  = (frObj << MOVE_FROBJ_BITS) | (fr << MOVE_FR_BITS);
    const frRank  = RANK[fr];

    switch (frPiece) {
      case 1: {
        // P
        
        // orth
        
        to    = fr + offsetOrth;
        toObj = b[to];
        
        if (toObj === 0) {
        
          if (frRank === promoteRank)
            addQPromotion(node, MOVE_PROMOTE_MASK | frMove | to);
        
        }
        
        // diag1
        
        to    = fr + offsetDiag1;
        toObj = b[to];
        
        if (CAPTURE[toObj] !== 0) {
        
          if (frRank === promoteRank)
            addQPromotion(node, MOVE_PROMOTE_MASK | frMove | (toObj << MOVE_TOOBJ_BITS) | to);
          else
            addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        }
        
        else if (toObj === 0 && to === bdEp)
          addQMove(node, MOVE_EPTAKE_MASK | frMove | to);
        
        // diag2
        
        to    = fr + offsetDiag2;
        toObj = b[to];
        
        if (CAPTURE[toObj] !== 0) {
        
          if (frRank === promoteRank)
            addQPromotion(node, MOVE_PROMOTE_MASK | frMove | (toObj << MOVE_TOOBJ_BITS) | to);
          else
            addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        }
        
        else if (toObj === 0 && to === bdEp)
          addQMove(node, MOVE_EPTAKE_MASK | frMove | to);
        
        
        break;
        
      }
      case 2: {
        // N
        
        to = fr + 25;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 25;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 23;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 23;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 14;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 14;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 10;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 10;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        break;
        
      }
      case 3: {
        // B
        
        to = fr + 11;
        while (b[to] === 0)
          to += 11;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 11;
        while (b[to] === 0)
          to -= 11;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 13;
        while (b[to] === 0)
          to += 13;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 13;
        while (b[to] === 0)
          to -= 13;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        break;
        
      }
      case 4: {
        // R
        
        to = fr + 1;
        while (b[to] === 0)
          to += 1;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 1;
        while (b[to] === 0)
          to -= 1;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 12;
        while (b[to] === 0)
          to += 12;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 12;
        while (b[to] === 0)
          to -= 12;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        break;
        
      }
      case 5: {
        // B
        
        to = fr + 11;
        while (b[to] === 0)
          to += 11;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 11;
        while (b[to] === 0)
          to -= 11;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 13;
        while (b[to] === 0)
          to += 13;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 13;
        while (b[to] === 0)
          to -= 13;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        // R
        
        to = fr + 1;
        while (b[to] === 0)
          to += 1;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 1;
        while (b[to] === 0)
          to -= 1;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 12;
        while (b[to] === 0)
          to += 12;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 12;
        while (b[to] === 0)
          to -= 12;
        if (CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        break;
        
      }
      case 6: {
        // K
        
        to = fr + 11;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0 && CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 11;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0 && CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 13;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0 && CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 13;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0 && CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 1;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0 && CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 1;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0 && CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr + 12;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0 && CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        to = fr - 12;
        if (ADJACENT[Math.abs(to-theirKingSq)] === 0 && CAPTURE[toObj = b[to]] !== 0)
          addQMove(node, frMove | (toObj << MOVE_TOOBJ_BITS) | to);
        
        break;
        
      }
    }

    next++;
    count++
  }

}

function isKingAttacked (byCol) {

  const list = wbList[(byCol ^ COLOUR_MASK) >>> 3];

  return isAttacked(list[0], byCol);

}

function isAttacked (to, byCol) {

  const b = bdB;

  let fr;

  // colour stuff
  
  if (byCol === WHITE) {
  
    if (b[to+13] === W_PAWN || b[to+11] === W_PAWN)
      return 1;
  
    var RQ = IS_WRQ;
    var BQ = IS_WBQ;
  }
  
  else {
  
    if (b[to-13] === B_PAWN || b[to-11] === B_PAWN)
      return 1;
  
    var RQ = IS_BRQ;
    var BQ = IS_BBQ;
  }
  
  const knight = KNIGHT | byCol;
  const king   = KING   | byCol;
  

  // knights
  
  if (b[to + -10] === knight) return 1;
  if (b[to + -23] === knight) return 1;
  if (b[to + -14] === knight) return 1;
  if (b[to + -25] === knight) return 1;
  if (b[to +  10] === knight) return 1;
  if (b[to +  23] === knight) return 1;
  if (b[to +  14] === knight) return 1;
  if (b[to +  25] === knight) return 1;
  
  // queen, bishop, rook
  
  fr = to + 1;  while (b[fr] === 0) fr += 1;  if (RQ[b[fr]] !== 0) return 1;
  fr = to - 1;  while (b[fr] === 0) fr -= 1;  if (RQ[b[fr]] !== 0) return 1;
  fr = to + 12; while (b[fr] === 0) fr += 12; if (RQ[b[fr]] !== 0) return 1;
  fr = to - 12; while (b[fr] === 0) fr -= 12; if (RQ[b[fr]] !== 0) return 1;
  fr = to + 11; while (b[fr] === 0) fr += 11; if (BQ[b[fr]] !== 0) return 1;
  fr = to - 11; while (b[fr] === 0) fr -= 11; if (BQ[b[fr]] !== 0) return 1;
  fr = to + 13; while (b[fr] === 0) fr += 13; if (BQ[b[fr]] !== 0) return 1;
  fr = to - 13; while (b[fr] === 0) fr -= 13; if (BQ[b[fr]] !== 0) return 1;
  

  return 0;

}


function evaluate (node, turn) {

  // init
  
  const numPieces = wCount + bCount;
  
  const wNumQueens  = wCounts[QUEEN];
  const wNumRooks   = wCounts[ROOK];
  const wNumBishops = wCounts[BISHOP];
  const wNumKnights = wCounts[KNIGHT];
  const wNumPawns   = wCounts[PAWN];
  
  const bNumQueens  = bCounts[QUEEN];
  const bNumRooks   = bCounts[ROOK];
  const bNumBishops = bCounts[BISHOP];
  const bNumKnights = bCounts[KNIGHT];
  const bNumPawns   = bCounts[PAWN];
  
  // draw?
  
  if (numPieces === 2)
    return 0;
  
  if (numPieces === 3 && (wNumKnights !== 0 || wNumBishops !== 0 || bNumKnights !== 0 || bNumBishops !== 0))
    return 0;
  
  if (numPieces === 4 && (wNumKnights !== 0 || wNumBishops !== 0) && (bNumKnights !== 0 || bNumBishops !== 0))
    return 0;
  
  if (numPieces === 4 && (wNumKnights === 2 || bNumKnights === 2))
    return 0;
  
  if (numPieces === 5 && wNumKnights === 2 && (bNumKnights !== 0 || bNumBishops !== 0))
    return 0;
  
  if (numPieces === 5 && bNumKnights === 2 && (wNumKnights !== 0 || wNumBishops !== 0))
    return 0;
  
  if (numPieces === 5 && wNumBishops === 2 && bNumBishops !== 0)
    return 0;
  
  if (numPieces === 5 && bNumBishops === 2 && wNumBishops !== 0)
    return 0;
  
  if (numPieces === 4 && wNumRooks !== 0 && bNumRooks !== 0)
    return 0;
  
  if (numPieces === 4 && wNumQueens !== 0 && bNumQueens !== 0)
    return 0;

  return netEval(node, turn);

}

const objHistory = new Uint32Array(15 * 256);

function addHistory (bonus, move) {

  const frObj = (move & MOVE_FROBJ_MASK) >>> MOVE_FROBJ_BITS;
  const to    = (move & MOVE_TO_MASK)    >>> MOVE_TO_BITS;

  objHistory[(frObj << 8) + to] += bonus;

}

const WB_OFFSET_DIAG1 = new Int8Array([-13, 13]);
const WB_OFFSET_DIAG2 = new Int8Array([-11, 11]);

const QS = new Uint8Array([0,0,3,3,5,9,0]);

function quickSee (turn, move) {

  if ((move & MOVE_SPECIAL_MASK) !== 0)
    return 0;

  const frObj   = (move & MOVE_FROBJ_MASK) >>> MOVE_FROBJ_BITS;
  const frPiece = frObj & PIECE_MASK;

  if (frPiece === PAWN)
    return 0;

  const to    = (move & MOVE_TO_MASK   ) >>> MOVE_TO_BITS;
  const toObj = (move & MOVE_TOOBJ_MASK) >>> MOVE_TOOBJ_BITS;

  const cx = turn >>> 3;

  const nextTurn = turn ^ BLACK;

  const p1 = (bdB[to + WB_OFFSET_DIAG1[cx]] === (PAWN | nextTurn)) | 0;
  const p2 = (bdB[to + WB_OFFSET_DIAG2[cx]] === (PAWN | nextTurn)) | 0;

  if (toObj === 0 && (p1 !== 0 || p2 !== 0))
    return -1;

  const toPiece = toObj & PIECE_MASK;
  const dodgy   = (QS[frPiece] > QS[toPiece]) | 0;

  if (dodgy !== 0 && (p1 !== 0 || p2 !== 0))
    return -1;

  return 0;

}

function isDraw () {

  if (repHi - repLo >= 100)
    return 1;

  for (let i=repHi-5; i >= repLo; i -= 2) {

    if (repLoHash[i] === loHash && repHiHash[i] === hiHash)
      return 1;

  }

  const numPieces = wCount + bCount;

  if (numPieces === 2)
    return 1;

  const wNumBishops = wCounts[BISHOP];
  const wNumKnights = wCounts[KNIGHT];

  const bNumBishops = bCounts[BISHOP];
  const bNumKnights = bCounts[KNIGHT];

  if (numPieces === 3 && (wNumKnights !== 0 || wNumBishops !== 0 || bNumKnights !== 0 || bNumBishops !== 0))
    return 1;

  return 0;

}

function formatBoard (turn) {

  const pieces = '.PNBRQKx.pnbrqk';
  var s = '';

  s += '  +---+---+---+---+---+---+---+---+\n';

  for (var i=0; i < 8; i++) {
    s += (8 - i) + ' ';
    for (var j=0; j < 8; j++) {
      var obj = bdB[B88[i*8 + j]];
      s += '| ' + pieces[obj] + ' ';
    }
    s += '|\n';
    s += '  +---+---+---+---+---+---+---+---+\n';
  }

  s += '    a   b   c   d   e   f   g   h\n\n';

  s += 'stm: ' + (turn === WHITE ? 'white' : 'black');

  var rights = '';
  if (bdRights & WHITE_RIGHTS_KING)  rights += 'K';
  if (bdRights & WHITE_RIGHTS_QUEEN) rights += 'Q';
  if (bdRights & BLACK_RIGHTS_KING)  rights += 'k';
  if (bdRights & BLACK_RIGHTS_QUEEN) rights += 'q';
  if (!rights) rights = '-';

  s += '  rights: ' + rights;
  s += '  ep: ' + (bdEp ? COORDS[bdEp] : '-');

  return s;
}

function formatMove (move) {

  let moveStr = 'NULL';

  if (move !== 0) {

    const fr = (move & MOVE_FR_MASK) >>> MOVE_FR_BITS;
    const to = (move & MOVE_TO_MASK) >>> MOVE_TO_BITS;

    moveStr = COORDS[fr] + COORDS[to];

    if ((move & MOVE_PROMOTE_MASK) !== 0)
      moveStr = moveStr + PROMOTES[(move & MOVE_PROMAS_MASK) >>> MOVE_PROMAS_BITS];

  }

  return moveStr;

}

function boardCheck (node, turn) {

  resolveAccs(node);  // node may still be lazy at verify points

  const a1 = new Int32Array(NET_H1_SIZE);
  const a2 = new Int32Array(NET_H1_SIZE);

  // hash
  
  var loH = 0;
  var hiH = 0;
  
  if (turn) {
    loH ^= loTurn;
    hiH ^= hiTurn;
  }
  
  loH ^= loRights[bdRights];
  hiH ^= hiRights[bdRights];
  
  loH ^= loEP[bdEp];
  hiH ^= hiEP[bdEp];
  
  for (var sq=0; sq<144; sq++) {
  
    var obj = bdB[sq];
  
    if (obj === 0 || obj === EDGE)
      continue;
  
    var piece = obj & PIECE_MASK;
    var col   = obj & COLOR_MASK;
  
    loH ^= loObjPieces[(obj << 8) + sq];
    hiH ^= hiObjPieces[(obj << 8) + sq];
  
  }
  
  if (loH !== loHash)
    console.log('*************** LO',loH,loHash);
  
  if (hiH !== hiHash)
    console.log('*************** HI',hiH,hiHash);
  
  // accumulators
  
  a1.set(net_h1_b);
  a2.set(net_h1_b);
  
  for (let sq=0; sq < 64; sq++) {
  
    const fr    = B88[sq];
    const frObj = bdB[fr];
  
    if (frObj === 0)
      continue;
  
    const off1 = IMAP[(frObj << 8) + fr];
  
    for (let h=0; h < NET_H1_SIZE; h++) {
      const idx1 = off1 + h;
      a1[h] += net_h1_w_flat[idx1];
      a2[h] += net_h2_w_flat[idx1];
    }
  
  }
  
  for (let i=0; i < NET_H1_SIZE; i++) {
    if (a1[i] !== node.net_h1_a[i])
      console.log('****** A1', i, a1[i], node.net_h1_a[i]);
    if (a2[i] !== node.net_h2_a[i])
      console.log('****** A2', i, a2[i], node.net_h2_a[i]);
  }

}

let statsStartTime = 0;
let statsNodes     = 0;
let statsMoveTime  = 0;
let statsMaxNodes  = 0;
let statsTimeOut   = 0;
let statsSelDepth  = 0;
let statsBestMove  = 0;
let statsBestScore = 0;

let multiPV      = 1;
let multiPVMoves = [];

// initStats

function initStats () {

  statsStartTime = now();
  statsNodes     = 0;
  statsMoveTime  = 0;
  statsMaxNodes  = 0;
  statsTimeOut   = 0;
  statsSelDepth  = 0;
  statsBestMove  = 0;
  statsBestScore = 0;

}

// checkTime

function checkTime () {

  if (statsBestMove && statsMoveTime > 0 && ((now() - statsStartTime) >= statsMoveTime))

    statsTimeOut = 1;

  if (statsBestMove && statsMaxNodes > 0 && statsNodes >= statsMaxNodes * 100)

    statsTimeOut = 1;

}

const BENCHFENS = [

"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkQ - 0 1",
"r3k2r/2pb1ppp/2pp1q2/p7/1nP1B3/1P2P3/P2N1PPP/R2QK2R w KQkq a6 0 14",
"4rrk1/2p1b1p1/p1p3q1/4p3/2P2n1p/1P1NR2P/PB3PP1/3R1QK1 b - - 2 24",
"r3qbrk/6p1/2b2pPp/p3pP1Q/PpPpP2P/3P1B2/2PB3K/R5R1 w - - 16 42",
"6k1/1R3p2/6p1/2Bp3p/3P2q1/P7/1P2rQ1K/5R2 b - - 4 44",
"8/8/1p2k1p1/3p3p/1p1P1P1P/1P2PK2/8/8 w - - 3 54",
"7r/2p3k1/1p1p1qp1/1P1Bp3/p1P2r1P/P7/4R3/Q4RK1 w - - 0 36",
"r1bq1rk1/pp2b1pp/n1pp1n2/3P1p2/2P1p3/2N1P2N/PP2BPPP/R1BQ1RK1 b - - 2 10",
"3r3k/2r4p/1p1b3q/p4P2/P2Pp3/1B2P3/3BQ1RP/6K1 w - - 3 87",
"2r4r/1p4k1/1Pnp4/3Qb1pq/8/4BpPp/5P2/2RR1BK1 w - - 0 42",
"4q1bk/6b1/7p/p1p4p/PNPpP2P/KN4P1/3Q4/4R3 b - - 0 37",
"2q3r1/1r2pk2/pp3pp1/2pP3p/P1Pb1BbP/1P4Q1/R3NPP1/4R1K1 w - - 2 34",
"1r2r2k/1b4q1/pp5p/2pPp1p1/P3Pn2/1P1B1Q1P/2R3P1/4BR1K b - - 1 37",
"r3kbbr/pp1n1p1P/3ppnp1/q5N1/1P1pP3/P1N1B3/2P1QP2/R3KB1R b KQkq b3 0 17",
"8/6pk/2b1Rp2/3r4/1R1B2PP/P5K1/8/2r5 b - - 16 42",
"1r4k1/4ppb1/2n1b1qp/pB4p1/1n1BP1P1/7P/2PNQPK1/3RN3 w - - 8 29",
"8/p2B4/PkP5/4p1pK/4Pb1p/5P2/8/8 w - - 29 68",
"3r4/ppq1ppkp/4bnp1/2pN4/2P1P3/1P4P1/PQ3PBP/R4K2 b - - 2 20",
"5rr1/4n2k/4q2P/P1P2n2/3B1p2/4pP2/2N1P3/1RR1K2Q w - - 1 49",
"1r5k/2pq2p1/3p3p/p1pP4/4QP2/PP1R3P/6PK/8 w - - 1 51",
"q5k1/5ppp/1r3bn1/1B6/P1N2P2/BQ2P1P1/5K1P/8 b - - 2 34",
"r1b2k1r/5n2/p4q2/1ppn1Pp1/3pp1p1/NP2P3/P1PPBK2/1RQN2R1 w - - 0 22",
"r1bqk2r/pppp1ppp/5n2/4b3/4P3/P1N5/1PP2PPP/R1BQKB1R w KQkq - 0 5",
"r1bqr1k1/pp1p1ppp/2p5/8/3N1Q2/P2BB3/1PP2PPP/R3K2n b Q - 1 12",
"r1bq2k1/p4r1p/1pp2pp1/3p4/1P1B3Q/P2B1N2/2P3PP/4R1K1 b - - 2 19",
"r4qk1/6r1/1p4p1/2ppBbN1/1p5Q/P7/2P3PP/5RK1 w - - 2 25",
"r7/6k1/1p6/2pp1p2/7Q/8/p1P2K1P/8 w - - 0 32",
"r3k2r/ppp1pp1p/2nqb1pn/3p4/4P3/2PP4/PP1NBPPP/R2QK1NR w KQkq - 1 5",
"3r1rk1/1pp1pn1p/p1n1q1p1/3p4/Q3P3/2P5/PP1NBPPP/4RRK1 w - - 0 12",
"5rk1/1pp1pn1p/p3Brp1/8/1n6/5N2/PP3PPP/2R2RK1 w - - 2 20",
"8/1p2pk1p/p1p1r1p1/3n4/8/5R2/PP3PPP/4R1K1 b - - 3 27",
"8/4pk2/1p1r2p1/p1p4p/Pn5P/3R4/1P3PP1/4RK2 w - - 1 33",
"8/5k2/1pnrp1p1/p1p4p/P6P/4R1PK/1P3P2/4R3 b - - 1 38",
"8/8/1p1kp1p1/p1pr1n1p/P6P/1R4P1/1P3PK1/1R6 b - - 15 45",
"8/8/1p1k2p1/p1prp2p/P2n3P/6P1/1P1R1PK1/4R3 b - - 5 49",
"8/8/1p4p1/p1p2k1p/P2npP1P/4K1P1/1P6/3R4 w - - 6 54",
"8/8/1p4p1/p1p2k1p/P2n1P1P/4K1P1/1P6/6R1 b - - 6 59",
"8/5k2/1p4p1/p1pK3p/P2n1P1P/6P1/1P6/4R3 b - - 14 63",
"8/1R6/1p1K1kp1/p6p/P1p2P1P/6P1/1Pn5/8 w - - 0 67",
"1rb1rn1k/p3q1bp/2p3p1/2p1p3/2P1P2N/PP1RQNP1/1B3P2/4R1K1 b - - 4 23",
"4rrk1/pp1n1pp1/q5p1/P1pP4/2n3P1/7P/1P3PB1/R1BQ1RK1 w - - 3 22",
"r2qr1k1/pb1nbppp/1pn1p3/2ppP3/3P4/2PB1NN1/PP3PPP/R1BQR1K1 w - - 4 12",
"2r2k2/8/4P1R1/1p6/8/P4K1N/7b/2B5 b - - 0 55",
"6k1/5pp1/8/2bKP2P/2P5/p4PNb/B7/8 b - - 1 44",
"2rqr1k1/1p3p1p/p2p2p1/P1nPb3/2B1P3/5P2/1PQ2NPP/R1R4K w - - 3 25",
"r1b2rk1/p1q1ppbp/6p1/2Q5/8/4BP2/PPP3PP/2KR1B1R b - - 2 14",
"6r1/5k2/p1b1r2p/1pB1p1p1/1Pp3PP/2P1R1K1/2P2P2/3R4 w - - 1 36",
"rnbqkb1r/pppppppp/5n2/8/2PP4/8/PP2PPPP/RNBQKBNR b KQkq c3 0 2",
"2rr2k1/1p4bp/p1q1p1p1/4Pp1n/2PB4/1PN3P1/P3Q2P/2RR2K1 w - f6 0 20",
"3br1k1/p1pn3p/1p3n2/5pNq/2P1p3/1PN3PP/P2Q1PB1/4R1K1 w - - 0 23",
"2r2b2/5p2/5k2/p1r1pP2/P2pB3/1P3P2/K1P3R1/7R w - - 23 93"
];

const PERFTFENS = [
  ['fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR             w KQkq -  0 1', 2, 400,       'cpw-pos1-2'],
  ['fen 4k3/8/8/8/8/8/R7/R3K2R                                  w Q    -  0 1', 3, 4729,      'castling-2'],
  ['fen 4k3/8/8/8/8/8/R7/R3K2R                                  w K    -  0 1', 3, 4686,      'castling-3'],
  ['fen 4k3/8/8/8/8/8/R7/R3K2R                                  w -    -  0 1', 3, 4522,      'castling-4'],
  ['fen r3k2r/r7/8/8/8/8/8/4K3                                  b kq   -  0 1', 3, 4893,      'castling-5'],
  ['fen r3k2r/r7/8/8/8/8/8/4K3                                  b q    -  0 1', 3, 4729,      'castling-6'],
  ['fen r3k2r/r7/8/8/8/8/8/4K3                                  b k    -  0 1', 3, 4686,      'castling-7'],
  ['fen r3k2r/r7/8/8/8/8/8/4K3                                  b -    -  0 1', 3, 4522,      'castling-8'],
  ['fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR             w KQkq -  0 1', 0, 1,         'cpw-pos1-0'],
  ['fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR             w KQkq -  0 1', 1, 20,        'cpw-pos1-1'],
  ['fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR             w KQkq -  0 1', 3, 8902,      'cpw-pos1-3'],
  ['fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR             w KQkq -  0 1', 4, 197281,    'cpw-pos1-4'],
  ['fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR             w KQkq -  0 1', 5, 4865609,   'cpw-pos1-5'],
  ['fen rnbqkb1r/pp1p1ppp/2p5/4P3/2B5/8/PPP1NnPP/RNBQK2R        w KQkq -  0 1', 1, 42,        'cpw-pos5-1'],
  ['fen rnbqkb1r/pp1p1ppp/2p5/4P3/2B5/8/PPP1NnPP/RNBQK2R        w KQkq -  0 1', 2, 1352,      'cpw-pos5-2'],
  ['fen rnbqkb1r/pp1p1ppp/2p5/4P3/2B5/8/PPP1NnPP/RNBQK2R        w KQkq -  0 1', 3, 53392,     'cpw-pos5-3'],
  ['fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -  0 1', 1, 48,        'cpw-pos2-1'],
  ['fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -  0 1', 2, 2039,      'cpw-pos2-2'],
  ['fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -  0 1', 3, 97862,     'cpw-pos2-3'],
  ['fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8                         w -    -  0 1', 5, 674624,    'cpw-pos3-5'],
  ['fen n1n5/PPPk4/8/8/8/8/4Kppp/5N1N                           b -    -  0 1', 1, 24,        'prom-1    '],
  ['fen 8/5bk1/8/2Pp4/8/1K6/8/8                                 w -    d6 0 1', 6, 824064,    'ccc-1     '],
  ['fen 8/8/1k6/8/2pP4/8/5BK1/8                                 b -    d3 0 1', 6, 824064,    'ccc-2     '],
  ['fen 8/8/1k6/2b5/2pP4/8/5K2/8                                b -    d3 0 1', 6, 1440467,   'ccc-3     '],
  ['fen 8/5k2/8/2Pp4/2B5/1K6/8/8                                w -    d6 0 1', 6, 1440467,   'ccc-4     '],
  ['fen 5k2/8/8/8/8/8/8/4K2R                                    w K    -  0 1', 6, 661072,    'ccc-5     '],
  ['fen 4k2r/8/8/8/8/8/8/5K2                                    b k    -  0 1', 6, 661072,    'ccc-6     '],
  ['fen 3k4/8/8/8/8/8/8/R3K3                                    w Q    -  0 1', 6, 803711,    'ccc-7     '],
  ['fen r3k3/8/8/8/8/8/8/3K4                                    b q    -  0 1', 6, 803711,    'ccc-8     '],
  ['fen r3k2r/1b4bq/8/8/8/8/7B/R3K2R                            w KQkq -  0 1', 4, 1274206,   'ccc-9     '],
  ['fen r3k2r/7b/8/8/8/8/1B4BQ/R3K2R                            b KQkq -  0 1', 4, 1274206,   'ccc-10    '],
  ['fen r3k2r/8/3Q4/8/8/5q2/8/R3K2R                             b KQkq -  0 1', 4, 1720476,   'ccc-11    '],
  ['fen r3k2r/8/5Q2/8/8/3q4/8/R3K2R                             w KQkq -  0 1', 4, 1720476,   'ccc-12    '],
  ['fen 2K2r2/4P3/8/8/8/8/8/3k4                                 w -    -  0 1', 6, 3821001,   'ccc-13    '],
  ['fen 3K4/8/8/8/8/8/4p3/2k2R2                                 b -    -  0 1', 6, 3821001,   'ccc-14    '],
  ['fen 8/8/1P2K3/8/2n5/1q6/8/5k2                               b -    -  0 1', 5, 1004658,   'ccc-15    '],
  ['fen 5K2/8/1Q6/2N5/8/1p2k3/8/8                               w -    -  0 1', 5, 1004658,   'ccc-16    '],
  ['fen 4k3/1P6/8/8/8/8/K7/8                                    w -    -  0 1', 6, 217342,    'ccc-17    '],
  ['fen 8/k7/8/8/8/8/1p6/4K3                                    b -    -  0 1', 6, 217342,    'ccc-18    '],
  ['fen 8/P1k5/K7/8/8/8/8/8                                     w -    -  0 1', 6, 92683,     'ccc-19    '],
  ['fen 8/8/8/8/8/k7/p1K5/8                                     b -    -  0 1', 6, 92683,     'ccc-20    '],
  ['fen K1k5/8/P7/8/8/8/8/8                                     w -    -  0 1', 6, 2217,      'ccc-21    '],
  ['fen 8/8/8/8/8/p7/8/k1K5                                     b -    -  0 1', 6, 2217,      'ccc-22    '],
  ['fen 8/k1P5/8/1K6/8/8/8/8                                    w -    -  0 1', 7, 567584,    'ccc-23    '],
  ['fen 8/8/8/8/1k6/8/K1p5/8                                    b -    -  0 1', 7, 567584,    'ccc-24    '],
  ['fen 8/8/2k5/5q2/5n2/8/5K2/8                                 b -    -  0 1', 4, 23527,     'ccc-25    '],
  ['fen 8/5k2/8/5N2/5Q2/2K5/8/8                                 w -    -  0 1', 4, 23527,     'ccc-26    '],
  ['fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR             w KQkq -  0 1', 6, 119060324, 'cpw-pos1-6'],
  ['fen 8/p7/8/1P6/K1k3p1/6P1/7P/8                              w -    -  0 1', 8, 8103790,   'jvm-7     '],
  ['fen n1n5/PPPk4/8/8/8/8/4Kppp/5N1N                           b -    -  0 1', 6, 71179139,  'jvm-8     '],
  ['fen r3k2r/p6p/8/B7/1pp1p3/3b4/P6P/R3K2R                     w KQkq -  0 1', 6, 77054993,  'jvm-9     '],
  ['fen 8/5p2/8/2k3P1/p3K3/8/1P6/8                              b -    -  0 1', 8, 64451405,  'jvm-11    '],
  ['fen r3k2r/pb3p2/5npp/n2p4/1p1PPB2/6P1/P2N1PBP/R3K2R         w KQkq -  0 1', 5, 29179893,  'jvm-12    '],
  ['fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8                         w -    -  0 1', 7, 178633661, 'jvm-10    '],
  ['fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -  0 1', 5, 193690690, 'jvm-6     '],
  ['fen 8/2pkp3/8/RP3P1Q/6B1/8/2PPP3/rb1K1n1r                   w -    -  0 1', 6, 181153194, 'ob1       '],
  ['fen rnbqkb1r/ppppp1pp/7n/4Pp2/8/8/PPPP1PPP/RNBQKBNR         w KQkq f6 0 1', 6, 244063299, 'jvm-5     '],
  ['fen 8/2ppp3/8/RP1k1P1Q/8/8/2PPP3/rb1K1n1r                   w -    -  0 1', 6, 205552081, 'ob2       '],
  ['fen 8/8/3q4/4r3/1b3n2/8/3PPP2/2k1K2R                        w K    -  0 1', 6, 207139531, 'ob3       '],
  ['fen 4r2r/RP1kP1P1/3P1P2/8/8/3ppp2/1p4p1/4K2R                b K    -  0 1', 6, 314516438, 'ob4       '],
  ['fen r3k2r/8/8/8/3pPp2/8/8/R3K1RR                            b KQkq e3 0 1', 6, 485647607, 'jvm-1     '],
  ['fen 8/3K4/2p5/p2b2r1/5k2/8/8/1q6                            b -    -  0 1', 7, 493407574, 'jvm-4     '],
  ['fen r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1   w kq   -  0 1', 6, 706045033, 'jvm-2     '],
  ['fen r6r/1P4P1/2kPPP2/8/8/3ppp2/1p4p1/R3K2R                  w KQ   -  0 1', 6, 975944981, 'ob5       ']
];

function bench (depth) {

  depth = depth || BENCH_DEPTH;

  silentMode = 1;

  let nodes = 0;
  let start = now();

  for (let i=0; i < BENCHFENS.length; i++) {

    const fen = BENCHFENS[i];

    //if (nodeHost)
      //process.stdout.write(i.toString() + '\r');

    newGame();
    uciExec('position fen ' + fen);
    uciExec('go depth ' + depth);
        
    nodes += statsNodes;
      
  }
        
  const elapsed = now() - start;
  const nps = nodes/elapsed * 1000 | 0;
  
  silentMode = 0;

  uciSend('nodes', nodes, 'time', elapsed, 'nps', nps);

}

function perftTests (maxDepth) {

  const t1 = now();
  let errors = 0;
  let tot    = 0;
        
  for (let i=0; i < PERFTFENS.length; i++) {
        
    const p = PERFTFENS[i];
        
    const fen   = p[0];
    const depth = p[1];
    const moves = p[2];
    const id    = p[3];
        
    if (maxDepth && depth > maxDepth)
      continue;

    newGame();
    uciExec('position ' + fen);
        
    const nodes = perft(rootNode, depth, bdTurn);
        
    tot += nodes;
        
    const t2     = now();
    const sec    = Math.round((t2-t1)/100)/10;
    const secStr = sec.toString().padStart(6, ' ');
        
    let diff = nodes - moves;
        
    if (diff) {
      errors += Math.abs(diff);
      diff = '\x1b[1m' + diff + '\x1b[0m';
    }
        
    uciSend(secStr, id, fen, depth, diff, nodes, moves);
  }
          
  const elapsed = now() - t1;
  const nps = tot/elapsed * 1000 | 0;
        
  uciSend('errors', errors, 'nodes', tot, 'nps', nps);
        
}

function evalTests () {
        
  let sum = 0;

  for (let i=0; i < BENCHFENS.length; i++) {
        
    const fen = BENCHFENS[i];
        
    newGame();
    uciExec('position fen ' + fen);
    const ev = evaluate(rootNode, bdTurn);
    uciSend(ev, fen)
    sum += ev;

  }

  uciSend('sum', sum);

}


// datagen
//
// Self play data generation in viriformat, mirroring cwtch's
// src/datagen.c so files from both engines can be mixed.  Node
// only.  Use bin/datagen to run multiple processes.
//

const DG_RANDOM_PLIES   = 10;
const DG_SEARCH_NODES   = 5000;
const DG_DRAW_SCORE     = 10;
const DG_DRAW_COUNT     = 10;
const DG_DRAW_PLY       = 40;
const DG_MAX_GAME_MOVES = 512;
const DG_REPORT_SECS    = 10;
const DG_FILE_PREFIX    = 'data';

// viriformat constants

const VIRI_TYPE_EP     = 1;
const VIRI_TYPE_CASTLE = 2;
const VIRI_TYPE_PROMO  = 3;

const VIRI_WDL_BLACK_WIN = 0;
const VIRI_WDL_DRAW      = 1;
const VIRI_WDL_WHITE_WIN = 2;

const dgLegal   = new Uint32Array(MAX_MOVES);
const dgGameBuf = new Uint8Array(32 + DG_MAX_GAME_MOVES * 4 + 4);

// rng (splitmix32, local to datagen, seeded per process)

let dgSeed = 0;

function dgSeedRng () {

  dgSeed = (Date.now() ^ Math.imul(process.pid, 0x9E3779B9)) | 0;

  if (dgSeed === 0)
    dgSeed = 1;

}

function dgRand () {

  dgSeed = (dgSeed + 0x9E3779B9) | 0;

  let z = dgSeed;

  z = Math.imul(z ^ (z >>> 16), 0x21F0AAAD);
  z = Math.imul(z ^ (z >>> 15), 0x735A2D97);
  z = z ^ (z >>> 15);

  return z >>> 0;

}

// dgTo64
//
// 144 board square to viriformat square; a1=0, h8=63.
//

function dgTo64 (sq) {

  return ((RANK[sq] - 1) << 3) | (FILE[sq] - 1);

}

// dgPackBoard
//
// viriformat PackedBoard from the current global board into buf[0..31].
// The wdl byte (30) is patched in at the end of the game.
//

function dgPackBoard (buf) {

  buf.fill(0, 0, 32);

  let occLo = 0;
  let occHi = 0;
  let idx   = 0;

  for (let i=0; i < 64; i++) {
    // a1, b1, ..., h8

    const sq  = B88[((7 - (i >>> 3)) << 3) | (i & 7)];
    const obj = bdB[sq];

    if (obj === 0)
      continue;

    if (i < 32)
      occLo |= 1 << i;
    else
      occHi |= 1 << (i - 32);

    let type = (obj & PIECE_MASK) - 1;  // PNBRQK = 0-5

    if (type === ROOK - 1) {
      // unmoved rook = 6
      if (sq === SQH1 && (bdRights & WHITE_RIGHTS_KING)  !== 0) type = 6;
      if (sq === SQA1 && (bdRights & WHITE_RIGHTS_QUEEN) !== 0) type = 6;
      if (sq === SQH8 && (bdRights & BLACK_RIGHTS_KING)  !== 0) type = 6;
      if (sq === SQA8 && (bdRights & BLACK_RIGHTS_QUEEN) !== 0) type = 6;
    }

    const nibble  = (((obj & COLOR_MASK) >>> 3) << 3) | type;
    const byteIdx = 8 + (idx >>> 1);

    if ((idx & 1) !== 0)
      buf[byteIdx] |= nibble << 4;
    else
      buf[byteIdx] = nibble;

    idx++;

  }

  // bytes 0-7 occupancy

  buf[0] = occLo         & 0xFF;
  buf[1] = (occLo >>> 8)  & 0xFF;
  buf[2] = (occLo >>> 16) & 0xFF;
  buf[3] = (occLo >>> 24) & 0xFF;
  buf[4] = occHi          & 0xFF;
  buf[5] = (occHi >>> 8)  & 0xFF;
  buf[6] = (occHi >>> 16) & 0xFF;
  buf[7] = (occHi >>> 24) & 0xFF;

  // byte 24 stm and ep

  const ep = bdEp !== 0 ? dgTo64(bdEp) : 64;

  buf[24] = ((bdTurn >>> 3) << 7) | ep;

  // byte 25 halfmove clock (see the rep comment in makemove.js)

  buf[25] = Math.min(repHi - repLo, 255);

  // bytes 26-29 fullmove and eval (0), byte 30 wdl (patched later), byte 31 extra (0)

}

// dgMoveToViri

function dgMoveToViri (move) {

  const fr = (move & MOVE_FR_MASK) >>> MOVE_FR_BITS;
  let   to = (move & MOVE_TO_MASK) >>> MOVE_TO_BITS;

  let type  = 0;
  let promo = 0;

  if ((move & MOVE_EPTAKE_MASK) !== 0)
    type = VIRI_TYPE_EP;

  else if ((move & MOVE_CASTLE_MASK) !== 0) {
    // king takes rook
    type = VIRI_TYPE_CASTLE;
    if      (to === G1) to = H1;
    else if (to === C1) to = A1;
    else if (to === G8) to = H8;
    else if (to === C8) to = A8;
  }

  else if ((move & MOVE_PROMOTE_MASK) !== 0) {
    type  = VIRI_TYPE_PROMO;
    promo = (move & MOVE_PROMAS_MASK) >>> MOVE_PROMAS_BITS;  // NBRQ = 0-3
  }

  return dgTo64(fr) | (dgTo64(to) << 6) | (promo << 12) | (type << 14);

}

// dgLegalMoves

function dgLegalMoves (out) {

  const node     = rootNode;
  const turn     = bdTurn;
  const nextTurn = turn ^ COLOR_MASK;

  node.inCheck = isKingAttacked(nextTurn);

  cache(node);
  genMoves(node, turn);

  let n    = 0;
  let move = 0;

  while ((move = getNextMove(node)) !== 0) {

    makeMoveA(node, move);

    if ((move & MOVE_LEGAL_MASK) !== 0 || isKingAttacked(nextTurn) === 0)
      out[n++] = move;

    unmakeMove(node, move);

    uncacheA(node);

  }

  return n;

}

// dgPlayMove
//
// Make a move permanently at the root.  The accumulator update lands
// in the child node, so resolve it there and pull it back to the root.
//

function dgPlayMove (move) {

  const child = rootNode.childNode;

  makeMoveA(rootNode, move);

  resolveAccs(child);

  rootNode.net_h1_a.set(child.net_h1_a);
  rootNode.net_h2_a.set(child.net_h2_a);

  bdTurn ^= COLOR_MASK;

}

// dgPlayGame
//
// Play one game and write it to fd.  Returns the number of scored
// positions written; 0 means the game was discarded.
//

function dgPlayGame (fd) {

  const buf = dgGameBuf;

  newGame();
  position('rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR', 'w', 'KQkq', '-', []);

  // random opening; + 0 or 1 extra ply to randomise stm

  const numRandom = DG_RANDOM_PLIES + (dgRand() & 1);

  for (let i=0; i < numRandom; i++) {

    const n = dgLegalMoves(dgLegal);

    if (n === 0)
      return 0;

    dgPlayMove(dgLegal[dgRand() % n]);

  }

  if (isDraw() !== 0)
    return 0;

  // save the starting position (after random plies)

  dgPackBoard(buf);

  // scored play

  let numEntries = 0;
  let drawCount  = 0;
  let wdl        = VIRI_WDL_DRAW;

  for (let ply=0; ply < DG_MAX_GAME_MOVES; ply++) {

    const stm     = bdTurn;
    const inCheck = isKingAttacked(stm ^ COLOR_MASK);
    const n       = dgLegalMoves(dgLegal);

    if (n === 0) {
      if (inCheck !== 0)
        wdl = stm === WHITE ? VIRI_WDL_BLACK_WIN : VIRI_WDL_WHITE_WIN;
      break;
    }

    // search

    initStats();

    statsMaxNodes = DG_SEARCH_NODES;

    go(MAX_PLY);

    let best = statsBestMove;

    const score = statsBestScore;

    if (best === 0)
      best = dgLegal[0];

    const whiteScore = stm === WHITE ? score : -score;

    // record move + score

    const viri = dgMoveToViri(best);
    const off  = 32 + numEntries * 4;

    buf[off]   = viri            & 0xFF;
    buf[off+1] = (viri >>> 8)    & 0xFF;
    buf[off+2] = whiteScore      & 0xFF;
    buf[off+3] = (whiteScore >> 8) & 0xFF;

    numEntries++;

    // adjudication

    if (Math.abs(score) <= DG_DRAW_SCORE)
      drawCount++;
    else
      drawCount = 0;

    if (drawCount >= DG_DRAW_COUNT && ply >= DG_DRAW_PLY) {
      wdl = VIRI_WDL_DRAW;
      break;
    }

    // play the move

    dgPlayMove(best);

    // draw checks

    if (isDraw() !== 0) {
      wdl = VIRI_WDL_DRAW;
      break;
    }

  }

  if (numEntries === 0)
    return 0;

  buf[30] = wdl;

  // terminator

  let len = 32 + numEntries * 4;

  buf[len]   = 0;
  buf[len+1] = 0;
  buf[len+2] = 0;
  buf[len+3] = 0;

  len += 4;

  fs.writeSync(fd, buf, 0, len);

  return numEntries;

}

// dgEta

function dgEta (ms) {

  const s = ms / 1000 | 0;
  const d = s / 86400 | 0;
  const h = (s % 86400) / 3600 | 0;
  const m = (s % 3600) / 60 | 0;

  const mm = (m < 10 ? '0' : '') + m;

  if (d > 0)
    return d + 'd ' + (h < 10 ? '0' : '') + h + ':' + mm;

  return h + ':' + mm;

}

// datagen

function datagen (directory, targetPositions) {

  dgSeedRng();

  const filename = directory + '/' + DG_FILE_PREFIX + dgRand() + '.vf';

  let fd = 0;

  try {
    fd = fs.openSync(filename, 'w');
  }
  catch (e) {
    console.log('error: cannot open ' + filename);
    return;
  }

  console.log('datagen: writing to ' + filename + ', target ' + targetPositions + ' positions');

  silentMode = 1;

  const startTime = Date.now();

  let totalPositions = 0;
  let totalGames     = 0;
  let lastReport     = startTime;

  // stop once the target position count is reached; the current game always
  // completes, so the final file slightly overshoots the target.

  while (totalPositions < targetPositions) {

    totalPositions += dgPlayGame(fd);
    totalGames++;

    const timeNow = Date.now();

    if (timeNow - lastReport >= DG_REPORT_SECS * 1000) {

      const elapsed   = timeNow - startTime;
      const pps       = elapsed ? (totalPositions * 1000 / elapsed | 0) : 0;
      const remaining = Math.max(targetPositions - totalPositions, 0);
      const etaMs     = pps ? (remaining * 1000 / pps) : 0;
      const pct       = (100 * totalPositions / targetPositions).toFixed(1);

      console.log('datagen: ' + totalPositions + '/' + targetPositions + ' positions (' + pct + '%) ' + totalGames + ' games ' + pps + ' pos/s [' + dgEta(etaMs) + ' left]');

      lastReport = timeNow;

    }

  }

  silentMode = 0;

  fs.closeSync(fd);

  console.log('datagen: done. ' + totalPositions + ' positions ' + totalGames + ' games written to ' + filename);

}
function uciSend () {

  if (silentMode)
    return;

  var s = '';

  for (var i = 0; i < arguments.length; i++)
    s += arguments[i] + ' ';

  //fs.writeSync(1, s + '\n');

  if (nodeHost)
    console.log(s);
  else
    postMessage(s);

}

// uciGetInt

function uciGetInt (tokens, key, def) {

  for (let i=0; i < tokens.length; i++)
    if (tokens[i] == key)
      if (i < tokens.length - 1)
        return parseInt(tokens[i+1]);

  return def;

}

// uciGetStr

function uciGetStr (tokens, key, def) {

  const lkey = key.toLowerCase();

  for (let i=0; i < tokens.length; i++)
    if (tokens[i].toLowerCase() == key)
      if (i < tokens.length - 1)
        return tokens[i+1];

  return def;

}

// uciGetArr

function uciGetArr (tokens, key, to) {

  var lo = 0;
  var hi = 0;

  for (let i=0; i < tokens.length; i++) {
    if (tokens[i] == key) {
      lo = i + 1;  //assumes at least one item
      hi = lo;
      for (let j=lo; j < tokens.length; j++) {
        if (tokens[j] == to)
          break;
        hi = j;
      }
    }
  }

  return {lo:lo, hi:hi};

}

// uciExec

function uciExec (commands) {

  const cmdList = commands.split('\n');

  for (let c=0; c < cmdList.length; c++ ) {

    let cmdStr = cmdList[c].replace(/(\r\n|\n|\r)/gm,"");

    cmdStr = cmdStr.trim();
    cmdStr = cmdStr.replace(/\s+/g,' ');

    const tokens = cmdStr.split(' ');

    let cmd = tokens[0];

    if (!cmd)
      continue;

    switch (cmd) {

      case 'isready': {
        // isready
        
        uciSend('readyok');
        
        break;
        
      }

      case 'position':
      case 'p': {
        // position
        
        if (ttSize == 1) {
          uciSend('info do a ucinewgame or setoption name hash command first');
          break;
        }
        
        let bd     = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR';
        let turn   = 'w';
        let rights = 'KQkq';
        let ep     = '-';
        
        let arr = uciGetArr(tokens, 'fen', 'moves');
        
        if (arr.lo > 0) {
          if (arr.lo <= arr.hi) bd     = tokens[arr.lo]; arr.lo++;
          if (arr.lo <= arr.hi) turn   = tokens[arr.lo]; arr.lo++;
          if (arr.lo <= arr.hi) rights = tokens[arr.lo]; arr.lo++;
          if (arr.lo <= arr.hi) ep     = tokens[arr.lo]; arr.lo++;
        }
        
        const moves = [];
        
        arr = uciGetArr(tokens, 'moves', 'fen');
        
        if (arr.lo > 0) {
          for (var i=arr.lo; i <= arr.hi; i++)
            moves.push(tokens[i]);
        }
        
        position(bd, turn, rights, ep, moves);
        
        break;
        
      }

      case 'go':
      case 'g': {
        // go
        
        if (ttSize == 1) {
          uciSend('info do a ucinewgame or setoption name hash command first');
          break;
        }
        
        if (bdB[0] !== EDGE) {
          uciSend('info do a position command first');
          break;
        }
        
        
        initStats();
        
        let depth     = Math.max(uciGetInt(tokens, 'depth', 0), uciGetInt(tokens, 'd', 0));
        let moveTime  = uciGetInt(tokens, 'movetime', 0);
        let maxNodes  = uciGetInt(tokens, 'nodes', 0);
        let wTime     = uciGetInt(tokens, 'wtime', 0);
        let bTime     = uciGetInt(tokens, 'btime', 0);
        let wInc      = uciGetInt(tokens, 'winc', 0);
        let bInc      = uciGetInt(tokens, 'binc', 0);
        let movesToGo = uciGetInt(tokens, 'movestogo', 0);
        
        let totTime = 0;
        let movTime = 0;
        let incTime = 0;
        
        if (depth <= 0)
          depth = MAX_PLY;
        
        if (moveTime > 0)
          statsMoveTime = moveTime;
        
        if (maxNodes > 0)
          statsMaxNodes = maxNodes;
        
        if (moveTime === 0) {
        
          if (movesToGo > 0)
            movesToGo += 2;
          else
            movesToGo = 30;
        
          if (bdTurn === WHITE) {
            totTime = wTime;
            incTime = wInc;
          }
          else {
            totTime = bTime;
            incTime = bInc;
          }
        
          movTime = myround(totTime / movesToGo) + incTime;
          movTime = movTime * 0.95;
        
          if (movTime > 0)
            statsMoveTime = movTime | 0;
        
          if (statsMoveTime < 1 && (wTime || bTime))
            statsMoveTime = 1;
        }
        
        go(depth);
        break;
        
      }

      case 'ucinewgame':
      case 'u': {
        
        newGame();
        break;
        
      }

      case 'setoption':
      case 'o': {
        
        const opt = uciGetStr(tokens, 'name', '').toLowerCase();
        
        if (opt == 'hash') {
          const mb = Math.max(uciGetInt(tokens, 'value', ttDefault), 1);
          console.log(mb);
          ttResize(mb);
        }

        else if (opt == 'multipv') {
          multiPV = Math.max(uciGetInt(tokens, 'value', 1), 1);
        }

        break;
        
      }

      case 'quit':
      case 'q': {
        
        process.exit(0);
        
        break;
        
      }

      case 'stop': {
        
        break;
        
      }

      case 'uci': {
        
        uciSend('id name Lozza', BUILD);
        uciSend('id author Colin Jenkins');
        uciSend('option name Hash type spin default', ttDefault, 'min 1 max 1024');
        uciSend('option name MultiPV type spin default 1 min 1 max 5');
        uciSend('uciok');
        
        break;
        
      }

      case 'perft': 
      case 'f': {
        
        const depth = uciGetInt(tokens, cmd, 0);
        const start = now();
        const nodes = perft(rootNode, depth, bdTurn);
        const elapsed = now() - start;
        const nps = nodes / elapsed * 1000 | 0;
        
        uciSend('depth', depth, 'nodes', nodes, 'time', elapsed, 'nps', nps);
        
        break;
        
      }

      case 'eval':
      case 'e': {
        
        const e = netEval(rootNode, bdTurn);
        uciSend(e);
        
        break;
        
      }

      case 'board':
      case 'b': {

        uciSend(formatBoard(bdTurn));

        break;

      }

      case 'bench':
      case 'h': {

        bench(uciGetInt(tokens, cmd, 0));
        break;
        
      }

      case 'pt': {
        
        perftTests(uciGetInt(tokens, 'pt', 0));
        break;
        
      }

      case 'et': {
        
        evalTests();
        break;
        
      }

      case 'datagen':
      case 'dg': {

        if (!nodeHost) {
          uciSend('info datagen needs node');
          break;
        }

        const target = Math.floor(parseFloat(tokens[2]));

        if (tokens.length !== 3 || !(target > 0)) {
          uciSend('usage: datagen <directory> <positions>');
          break;
        }

        datagen(tokens[1], target);

        break;

      }

      case 'net':
      case 'n': {
        
        uciSend('i_size, h1_size', NET_I_SIZE, NET_H1_SIZE);
        uciSend('qa, qb', NET_QA, NET_QB);
        uciSend('scale', NET_SCALE);
        
        uciExec('u');
        uciExec('p s');
        uciExec('e');
        
        break;
        
      }

      default: {
        
        uciSend('unknown command', cmd);
        
        break;
        
      }
    }
  }

}

const nodeHost = (typeof process) != 'undefined';

if (!nodeHost) {
  onmessage = function(e) {
    uciExec(e.data);
  }
}

const fs = (nodeHost) ? require('fs') : 0;

let silentMode = 0;

function initOnce () {

  // init ADJACENT
  
  ADJACENT[1]  = 1;
  ADJACENT[11] = 1;
  ADJACENT[12] = 1;
  ADJACENT[13] = 1;
  
  // init net
  //
  // IMAP is used to map a piece+colour to an offset in the flat weights array.
  // Used when updating the accumulators.
  //
  
  for (let i = 0; i < 64; i++) {
  
    const j = B88[i];
  
    IMAP[(W_PAWN << 8) + j]    =   (0 + (PAWN-1)   * 64 + i) * NET_H1_SIZE;
    IMAP[(W_KNIGHT << 8) + j]  =   (0 + (KNIGHT-1) * 64 + i) * NET_H1_SIZE;
    IMAP[(W_BISHOP << 8) + j]  =   (0 + (BISHOP-1) * 64 + i) * NET_H1_SIZE;
    IMAP[(W_ROOK << 8) + j]    =   (0 + (ROOK-1)   * 64 + i) * NET_H1_SIZE;
    IMAP[(W_QUEEN << 8) + j]   =   (0 + (QUEEN-1)  * 64 + i) * NET_H1_SIZE;
    IMAP[(W_KING << 8) + j]    =   (0 + (KING-1)   * 64 + i) * NET_H1_SIZE;
  
    IMAP[(B_PAWN << 8) + j]    = (384 + (PAWN-1)   * 64 + i) * NET_H1_SIZE;
    IMAP[(B_KNIGHT << 8) + j]  = (384 + (KNIGHT-1) * 64 + i) * NET_H1_SIZE;
    IMAP[(B_BISHOP << 8) + j]  = (384 + (BISHOP-1) * 64 + i) * NET_H1_SIZE;
    IMAP[(B_ROOK << 8) + j]    = (384 + (ROOK-1)   * 64 + i) * NET_H1_SIZE;
    IMAP[(B_QUEEN << 8) + j]   = (384 + (QUEEN-1)  * 64 + i) * NET_H1_SIZE;
    IMAP[(B_KING << 8) + j]    = (384 + (KING-1)   * 64 + i) * NET_H1_SIZE;
  
  }
  
  netLoad();
  
  // init nodes
  
  for (let i=0; i < nodes.length; i++) {
    nodes[i] = new nodeStruct();
    seal(nodes[i]);
    nodes[i].ply = i;
  }
  
  for (let i=0; i < nodes.length-1; i++)
    nodes[i].childNode = nodes[i+1];
  
  for (let i=1; i < nodes.length; i++)
    nodes[i].parentNode = nodes[i-1];
  
  for (let i=2; i < nodes.length; i++)
    nodes[i].grandparentNode = nodes[i-2];
  
  // init LMR_LOOKUP
  
  for (let p=0; p < MAX_PLY; p++) {
    for (let m=0; m < MAX_MOVES; m++) {
      LMR_LOOKUP[(p << 8) + m] = (1 + p/5 + m/20) | 0;
    }
  }
  
  // init ALIGNED
  
  for (var i=0; i < 144; i++) {
    ALIGNED[i] = new Int8Array(144).fill(EDGE);
    for (let j=0; j < 64; j++) {
      ALIGNED[i][B88[j]] = 0;
    }
  }
  
  for (let i=0; i < 64; i++) {
  
    const to = B88[i];
    const a  = ALIGNED[to];
  
    let dir = 1;
    let from = to + dir;
    while (a[from] !== EDGE) {
      a[from] = dir;
      from += dir;
    }
  
    dir = -1;
    from = to + dir;
    while (a[from] !== EDGE) {
      a[from] = dir;
      from += dir;
    }
  
    dir = 12;
    from = to + dir;
    while (a[from] !== EDGE) {
      a[from] = dir;
      from += dir;
    }
  
    dir = -12;
    from = to + dir;
    while (a[from] !== EDGE) {
      a[from] = dir;
      from += dir;
    }
  
    dir = 13;
    from = to + dir;
    while (a[from] !== EDGE) {
      a[from] = dir;
      from += dir;
    }
  
    dir = -13;
    from = to + dir;
    while (a[from] !== EDGE) {
      a[from] = dir;
      from += dir;
    }
  
    dir = 11;
    from = to + dir;
    while (a[from] !== EDGE) {
      a[from] = dir;
      from += dir;
    }
  
    dir = -11;
    from = to + dir;
    while (a[from] !== EDGE) {
      a[from] = dir;
      from += dir;
    }
  
  }
  

}

WEIGHTS_B64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA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0v/u/8D/CAAAAPv/DgD1//f/BQD6//3/KQALAPX/6P/2//H///8MACAADwDe//T/DgAKAO//CwDR/4P/6P8EAOv/r/8IANf/AgDr/wAADQD0/wkAFAAGABQAGAD//wUA7/8MAO//Xv/+//3/xv8IAP3/BgAHAMj/+P8EAPv/AQAOAAwA8/+S/9b/FACQ/wwAIwD+//P/AAD8/wYABQABAAgA9v8SAAMACwD9//n/+f8dAO7/DgD4/63///8CAAwAAwD0/+//AQANAO///v8GAP3/AAAHAO//+P8AABIA9v/h////BAAXAAAAAAACABgA9/8HAPb/AADj//v///8PAPr/DAABAPv/8//w/wMA9v/+/+f//P/b/xAA9v/9/zgAAgAIALj//f////P/9P8FAPX/sP8HAJv+2f/9//f/BAAIAAcAAQD0//n/w//1//v/AQANAAkA9//o/xUA6P8LAOX/CwD6/wIA/v/4/+j//v8QAAoABAD//wgA1ADy/woAAAAFAA8A5f/2/+j/9P/3/wsAEAAYALX/9v8BAPr/1f8AAPP/AgAAAAAAAQD2////9//B//n//v/4////7P/2/wIAof/7/wUAAAAAAPb/AQDw/+P/cf/7//v/7f///wQA/P/P//v/AADD//7/BwALAPD/4P/2/9//zf/9/wYA8/9iAOj/kv8HAPn/BgAGAN7/EgD6//z/AADc/woA6P+//+7/8f/9/wcAHgD6//v/CQAIAAUAIQAIAMv/k//v/wEA4P8LAPn/5f8EAPD/9/8BAPr/EAAUAA8AEADJ//n/6//1/wAADADs//b/+v/O/w8A9f8IAAMAwf/1//v//f/9/w8A8/8PAAgA8f8AAGL/+v8eAAYA7v/9//X/CQAGAAcADgD7/wAA//8PAOD/AgAFACEAEQAPAOj/xP/v//z/FAABAO//8v/+/93//f8EAAQA7v8AAAcABgDx////AAD1/9X/Tv8BAAAA4//m/wYABAD+/xYAAAACAPH//P/d//X/9f/9/wEA+f/4/+n/BgD3/w8AIwAJANT//v/H//j/yf8IAAwAt/8GAAMA/v/8//b/+v+6/wIAGgAEAAQA8/8QAAgAAgDo/+//AQD5/wIACAAGAAsA+f/y////AADP/wIA9P/5//r///////r/CwD9/wUACgAAAAQABQD3/wQAAQDr/wIAGgDc/wsAz//x//v/FAAFACQAm//o//T/+v/Z/wIA6v8MAAgABgAUAPT/5//v/9T/+v/8////+/8KAAAAAwD+/woACQD8//3/CwDy/wgADwBDAAAA4f/m//r/AgAAAL7/FwD8/wgACADY/xoA///o////FAAAAA0ACAAMAN//BAC//97/3f/9/wsA5f8BAAAA/f8AAAcAEAD0/8H////3/ygAAgAeAPP/+f/9/xIAFADu/xMA0/+x/9n/+P/i/wEA3//8//r//P/8/wUA+/8NAA0ACQALABcABwDe//b//f/v/wQAAwAAAMP/AAD5//7/1f/L/xUABgDx/xT/DwDb/wUAAAAJAPf/zf8aAB4AGQD6//X//P8AAAoAUQAEAP//IQAOAA8A9//1/w0AIgD//w4A8P/Y/wAABwAgAPj/9//w//H/kP++/wQADQD///3/BgAFAP///f/x//r/8v/2/wAADwADAOj/BwAFAHIABQACAPv/+//3/+v/AADw/x0AAQD1//X/8/8AABMANADh/wQA3v/e/xIA3P8EAPr/BgC3//v/BQDz//b/+P/0/7v/BgD0/wEABADw/w4A/v8DALYA9P/9//v/9f8QAO7/EQDu//b/+v8VANf////s/wEA+v8AAAUA+P/1/wIAAAAHAP3/+v/R//X/AgD//woAAQATAOr/HgD7//H/+f8HAPP/FwCS//X/AQD5/9f/+v/v/wIA//8FAAMAAAD9/wgAx//0//r/9P/8//7/AAAAAPv/AgAFAPf///8FAM7/8/8AAGf//v/x//r/+v/4//T/sP/2//z//v/2/yQAFwD///v/FADv/wMABgD0/wUADAACAP7//f/Y/w4AAQDj/wYA/f8FAAAA/f8pAPb/pP/5/+f/BQAEACYA/P///wIAFQAPAAAABwDc/4L/4P8AAOr/CQDh/woA9/8AAPz/AwAHAMX/FQD7/xUAAAD2/v7/+f8DAAQA/v8IAAEAwP8AABEA7f8VANP/BwAIAPv/AgAHAPv//f8AAAgADwDd/wYAKgAnAPv/DAD2/wMAGACL/9D/AAD6/wAAYP/x//v/CgAnAAUA+P/x/+b/5P/7/w0A8v/3/+//AACQ/wEA/f8BAPn/+/8HAAQA/P8v/wAAAQDz//j/AAAQAAQA7f8MAAUABgD3/wgA9P////L/8P8EAPP/LAANAAAA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BADx//r/4/8HABAA9P/v/wEA9v/2/woAJQADAPr/CgAbAAUADQAHANz/lv/s/wAA+P9S/wwA0v8IAOv//P8hAAsACgASAAoAFQAOAP//CgDg/yQA+//p//v/+//H/woA/v8FAAUAzP8GAAAAFAAAAA0AHgD9/3YAl/8pAIf/AwAkAAoA9v8PAAAAKAAIAAIADQD0/wkAAwAIAAMA9v/2/yAA1P8OAPn/rf8AAP//+v8DAPr/7v8IAAwAAAAAAAsA/P/1//7/7P8EAAMASADg/+n/AwAAABYA+v8RAAUAuP/7////7f8FAPj//P/+/8z/EAACABkA/////wIA//8AAAEAAwD7/9n/DADz////FP8KAAQAov8XAAAA8f/o/x0A9/+1/wcAUwAUAP3/AAAMAAUA7P/9/xIA+f8AAP7/7P8tABAA8v/1/8r/KAABAOv/8P/5/wEAAQADAPD/LQD+/wAAAACi/+7/DQDz//7/kAAKAPn/CwDt/w4A9P/5/wEA+f8AABcAkf/0/wEA+f/f/wgA+f8DAAcA///+/+//AgD+/83/8v8HAPr/CQDp/wYAUP///+X/BwD7/wAAAgANAAIAEQAEAOb/JADn//v/aP/7/8n/FgAAACUA/f8MABkACgCz//7/9P/q//r/AwD+/9D/4f/c/2//AAAAAA4A9P8TAP//+P8AAP//CQD3/+f/8//x//r/CwAiAP3/4//l/xcABwDe/wsA0v+Q/+X/AgDx//X/BwDX/wAA5f/7/yIACAAKAAgAFAAQAP3///8AAO7/DgDp/1b/+v/7/8j/CwD9/wUA/f/P//z//f/7////EQD+//L/AADT//f/i//3/yAACwD0/wAA+/8HAAcAAAAJAPr/BQD+/wcACADw//P/HQABAP7/8f+o/wUABQALAAIA5v/v/wMADQD//wEADwD0/wIABAD1//3//v8QAPn/1/8GAAcADwD7/wMABgAkAPj/CwDx//7/5v/9////FwD+////BwABAPL/9f/6//X/BABa//v/3P8wAO///v8xAAIACQCv/+j/8//x/+7/+v/x/7b/BwCg/tv/CgAAAAIAAQAKAP7/BADv/7//9v8CAAYAHAAWAPP/7/8TAOj//v/y/wcA/P/6/wwA//++//r/DAD7/wAA//8KACMA///2//z//f8UAOj/FgDl//H/+P8QAA0AHACL/+7//v/6/+H/BQDw/wkAAwD6/wcA8P/3/+z/y//y/wgA/v8GAP3/+/8EAGn/7/8QAP3//v8RAAIA8f/2/wUA+f8BAOn/+v/7//3/yv/7//v/z/8AAAAADwD8/9T/AQDt/8f/AwD9//v/TQDe/5b/HgD4/wEABADz//r/6//5/wEAEQAOAP3/zv/5/+3/+v8IAB8A9//8/w4AFAAQABwABwDG/5f/8f8CAPf/DAAFAOH/DQD2//T/JQAWAAMACQAMABQAk/79/+f/8v8FAAwA8//7//3/yv8KAPn/AAD7/8n/BQD2//b/+/8NAOz//P8FAOr/+f+v/xcAIAD1//L/CgD4/wsABwD5/wkA9/8UAPn/AADY//T/9/8gABEACQDr/8P/8//+/xsA///x//P/AQAAAK///v8PAPf/AAAFAPD/9P8AACEA8//N//z//f8FAOz/3f8NAAMA7v8VAPf//v/w//T/6v/0/+X/+/8CAAEA9P/4/wQA/v8oADMA/f/i//L/wP/3/zz/AAAKAKf/EADx//f//P/0//X/wP8GAEIA/v/z//P/DwD2//3/5/8HAPv/9//9/wkA+f8bAPD/8//t/wMAtf/9/+v/5v/9/+//AQD5//z//f8GAAwA/f/6/wEA+v/1//n/9P8eAA8A4f8XAOH/8//6//3/AAAUAIT/7v8AAPr/4P/4/+r/BwAKAPr/DgDz//b/8P/M//L/BQD6//7/+P/5//7/9//3////BQD6/woA8/8SAC8AVwD5//n/0v/5//T//v/C/x8A/P8fAPn/Dv8LAPv/uP8FAAoA4//6//z/+v/V//7/2f+3/9//+v/5/+T/EAD1//f/AQDz/w8A+P/O//L/7f8aAAUAJQDr//H/BwAZAAYA4/8MAM7/jf/u//v/8f8KANv//P/j//7/+v8dAPz/AgABAAoAFQAAAAUA3//6/wMA6//9//v//P/H/wgA+f8BAAoA1/8fAAIA+f9Y/wYA7f8TAAMAAAD+/8v/AQAnABcA9f8AAPv/CgAIAAQA/v/x/wcAAAACAA0A5/8CACUABgAPAOH/4v/6/wEAIQAFAPD/8f/8/5b//f///wYA9//5/wAA/f/r/97/9v/4/9n/RP8DAPv//f/j/wkACAAmAA8AAgD+/+3//P/h////7P/5/xIA/v/5//z////b/yYAXf8AANn/6v/p/0D/CAD6/woAzf/+/wAA9f/0/wAA9P+7/wsA+////wQA8P8KAPz/AQD2//n/9v/1//b/CgD5/xwAbwDx/wAAEAC3//j/9f/s//r/CwAFAO///v/8/wcAAwAAAO3/zf8GAAAA/v8KAAAAFADw/xcA7//h//X/HAD6/xoAgf/0/wUA/f/d/wIA8P8SAAoA//8PAPL/uP/z/8r/8P8GAPL//P/6////AAAEAAAAEQD1//r/EQDZ/9z/AQD///7////t//j//P/v/7L/AQD9//L/8P83AAYA/f8AABcA7/8BAAQAAAD+/xEAAgD+/wIAxP/p//r/5f8AAPP//P8AAP3/BQD//57/+//j/wQABQAjAAIAAAARABcACgAOABMA0/+H/+r/8//w/wsA1v8MAAIA+//7/yEA/v/y/wQA7f8OAAEA2P7r//f/AAAKAP7//v///8X/BgACAPP/+v/h/w8A///6/wYABQD1/wYAAgAIAAsA3v8QACsALAD0/wAA+v8MABkAcf7I//3/AQAAAPL/4v/z//b/IwAVAOP/8f/w/9T/+f8aAP3/9//x/wEAk/8BAPb/HADz//b/BAABAP7/CwD//wAA4//0//v/BgACAN3/DAAGAPL/AAAMAPP//f/o//X/BgD6/y8AGQD+/wAA/f8EAG8AJAAAAAAA2f/h/wIAQgAEAPf/BgCj/wIACAD1//7/+f/1/7z/BAADAAkACQDt/wYA/f8BAPz/FQD7//r/7/8SAPf/IgAGAPj/BAAWAOn/AADr//X/+v/j/xcA/P/8//3/8/8RAAIA+f/e////EwABAO//+/8PAPb/HAAIAAkA/v8JAO7/HgB6//r/2//7/9D/CQD1/wkA/f8AAA4AAQD/////1//n/w8ACAD///z/AwAHAAEAAwAEAP7/+/8KAML/CAAHAPv/AwD8/+7/+v///woAsv8DAPv/AwAMACz/DQD8/wsAGgD4/wAAAwCa//7/AAAKAAcA/v/b/7n+/f/k//H/+/8EAAAAAAC4/wcArf/6/+z/+P/8/yoA/f8AAP//KAAAAPv/+//f/4r/6P8DAPX/CQCx/w4A/f8AAPv/IgABAOn/FADw/xAAAAAHAAsA9P8AAAwABAAUAP7/xf8TAA4AZf9FAOn/HgAAAPr///8FAPn/DAAKAAYAFgDh/wkAMAD5//7//P/5/wgA3P/g/43/9P/3/wEAUgABAOz/AgAxAAcAJAD3//H/FQD6/xcAVf8FAO3/8P/s/wEA/v8PAPb/+f////3/EADu//z////q/wAAvf8LAPv/DQAMAAYAFQD4/wwA+P/+/wMADwAMAP//DAATAAAAAQD7/wMA2v8WAAkABgDV/+j/DwAH/wIA/P8DAKz/AQAJAP//DQD+//z/wf8OAAUACAABAPH/FwAFAAAABQAVAAAA//8DAA4A+f8vAA0A9/8AABUAAgD9//P/5P/+/yMADQAAAP//AgAJABYABgDy/8j//v8EAAMAJAAAABEAAAAKAA0A8////wwA9P8qAGD/AADg//z/2v8LAPX/BwAAAAAADAD7//7//v/K//D/BgACAP///P8FAAYACwAEAA8A7/8AAAgAWv8AAAUACQAFAAMAFAD+/wIA9/+//wAAAwABAOz/DQAJAAEADAAJAPz/AQABAAUAAgAGAAgABgAAAPT/EAD5/+f/AwD9/wkA/v8AAPj/BwCp/wQA9/8MAPr/IgAGAAQAEwAhAAEA/v+Z/+z/oP/n//3/+/8LAOn/AwADAP7/AAAMAA4A+f8TAOX/CQAFAP7/DQD2/wAAEAAKAPn+KADQ/+v/HQD4/yUA6v8XAAIABAD9/wIAAgAEAAgACgAPAOL///8iACQADAAAAP//CwAZ/yIArv/4//z/Af8PAAkA+f8CACcAAAD4//7/9P/0//j/FgBhAP3/8/8AAAAAAgAAAAcA+//4/wAA+/8OAAkAAAAAAPn/BACk/wYA/f8DAAwA8/8AAHT/AwAAAAAAAAANAAIAIAAMAAMAAwD5/xMABQAAAAQADAD5/8b/AQAGAAgABgASAAIAJ/8IAAwA/v/3/wcACwCe/w8A/v8TABAA//8PAAYAg/8GAAcAAAD//wMAuf8FABoA+//2/wAAIgADAAEA8P/0/wAA/v/v////DADs/wkACgAlAA8ACQAHAAIACQAAAAsAlP/y/yMADgAFABz//v8MACQAlf/3/wcA+v/P////BgAFAAoA/f/z//f/BAD8/9X/7P8WAPv/AgAAAPr/EAAOAPj/HQAMAFAAEAASAAsACwAIAAAAIwD6//7/DwD//8//HwD8/wAABAAJAAoARwAJAA4AAAANAAIABwD9/wkAAAAPAAMA/P///xsA/v8IAPv//P8iAAUADgAFAOL/CAD6//r/EAAjAAsAAAD4/ywACwDw/woA5P+y//b/9v/u/w0ADADi/wIA/f///yUA1v8SABAACAAJAAsAAAADAO3/LwD1/w4A/f/+/9P/EgAHAAwACADd/xAA/v/x/wMADwAqAAQACQDv/wsAq/8HADMACwD2/wEAAQDw/wwACAAMAP//BAAEAAcACgD2//P/HQAqAAwACgCO//3/AgDl/wUA/f/x/wcABAD7/wQABwABAE8AAgCw/wkABwAbACQAAAAEAAIAEgD9/wAAEQAdAAAA6v/u//X//////wQACAANAAgADwD///T/CQAAAAIACQAKAPn/yP8KAAYABgARABUABAAr/wsADgD6/+H/LQDs/6n/CwDr/zP/CQD6/xoABQCBAAgACAAJAP7/BgDb//3/LwD9//r/BQAbAAYABgDx/wYA+/8AAP3/GgD3/+7/FwD2/3z/EQAJAAgA/f8RABQAAADv//X/KQAEAAEAHAD6/wUAIAAK//T/BQD8/9H/9//7/wYADwD6//7/AAAHAP3/0P/o/xoA+v8OAAAAdv9jABIAOwAfAAgA9f8PAA8ADQACAAkACwBBAO7//f8KAP3/zv////z/BwAAAAoABwDb/+v/DAD4/////v8IAPv/DwC//wYA/v8AAAcAFgD9/w8ACgD+/9r/AwAQAPv/6P////n//f8HACgA5f8hABUAIQANAPb/DADg/5L/8f/+/+7/5/8JANX//v/w/wAAOwDv/w4AEAAJABEADQD8/wcA5P86APv/8f/9//r/zf8TAAUACAAHANT/DwAAABAABQAOAAMA+v8PAJr/9f+O/w4AIgASAPj/AQD9/woADQAHAAgA+P8HAP//CwARAPb/+v8fAPz/FQD8/5b//v8FAM//AQDr//D/DAAEAAgAAAACAAAAE/8EAEIACQAFAAYABf/t/woA+f8YAPn/CQAQAOv+AAD9//v/+v8CAPz/BwCa/gAABQASAAAA/v8DAAAABAAAAAsA+//R/zAAAQAIABQACAAEACr/EgAAAPP/2P8UAPn/rv8PAB4ATgACAAAAAgAAAKf/AgD9//b/BAD//9z/IwAcAPb/7f/7/yMAAAAHAPT/EgACAAAA+//T//D/BAAHAAEA//8AAAgA8v8EAPT/DgD8/wkA+v8nAPv/AAAEAOT/AgAbAAj//v8FAP3/4P/0//3/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AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA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3f8VACgACwDo/2L/BQAVABEA+//2/xwAYv8OAMj/8f/u/+D/6f+n/x8A/v/x/xoA7v8GAPv/+f8AAOj/+f/W//z//P8AAPv/8P/f////JQDq//H/0f8BAN//6v8UAN//IAAMACgAEADm/9r///8FAPH/AAC1//3/2P8HAPb/HQAKAMf/FAD6/x4ADv/w/wAAyv8y/xoA6P/x/wQAv//n/+r/BgD7/5j/6//6//L////1/woAtf8RAOX/AQAIAPj/DAAhAAMA5//7//n//v/0//L/zv/b/xMA8P/1/wcA9P/+/w8Avv8SAP//8P8QANX/GQDT//n/8//e//X/2f/6/9v/wv/8/+X/2//u/9P/MgDd/5D//f8sAOz/pf/C//j/8f8bAIb/BQDm/yMAJQDl/9P/7f+0/+b/AgCv/wQA0//x/w4A3P8PABYA7v9dABIA4/8HAO//CwD6/9//6f/q/0QAJAAVAAIA+P+e/9v/4v/w//b/DQAFAPX/BwDM/9T/k/8AALf/BgDv//r/5/9o/wYA9//t/+3/IQD8/xEAAADs//T/DQAHAPn/+v/Y/wMA7v8NAAYA/v+o/wwA7P8XAPT/GADh/+v/9f/n/+X/HQDW/+P///8EAP3/BQACABIAEwDm//f/LADX//f/7f/6//3/5f/o/wgAEgDi//r/FADd/xEABgAPAOv/Z/8QAB4AFQAMAAAAFgB5/xYAo//v//f/7v/5/6P/GQANAAwAGQDk/wsA8f/9/woABAD2/+3/AQAnAO7/HQDm/9v/DAARAOr/CwDY/woA4//3/xQA2v/q//X/AAALACcAAQAGABUADQAVAIT//P/u//v/GwAhAAEABgAHAOr/HwAF/wAA9P/H/1L/GQDi//j/CQDW//D/2P///wQApP/m/wIACAD//wAArP8dAAEADQAJAA8A+P8UACMAAgD0//X/9f8iAOP/7P88ANf/DwDx//3/AADz/wkAEwDq////8//n/woA6P8VAN//9v/v/8L/CgDX////8v/T//b/4f/e//z/CAAiAO7/P//0/wQA1v+4/8T/AgDz/wgAX/8aAPL/FgAlAOP/yf8AALv/xP/h/7X/CwDu//L/GADp/wgAFwAIAGQAQADo/wcA6f8eAOf/3f/0//3/PwAnAAgACAAAAJz/6f/R////6v8PABEAEAATAO7/FgCP/w8A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FADd/xoA9f////D/6f8rAN7/9P/i/7//JQDc//b/7f+0/x8A8v9r/wYA/P/k/6//y/8PAA4ADQB//9v/7P8FABgA+v/F/x4AqP8OAOP/5f/q/wkA8v/+/wcAHwAGABEAUwD1/xEA7P/K/woA8f/4/+n///9JAB8ABAAKAAgAnf8AALj/EwAPAPf/AAAmABoABQAFAKn//v/Y/xcAo//K/9z/EgD7/wYA3v/t/wAA///8/+n/v//x/w4Asf/v/xMAwv8hABEAHQD//9r/o/8dANL/CAA8APv/7f/c////BwB8/xUA+v/6/xcAJQARABQADQD7/zIABADu/xQA6f8pAAUADgAXAAIAFgD3/xgACwDd/zoAv/8SABgADwAAABX/GQAcABkACgAAAPP/XP/0/xAAGQD0/9j/tf+z//7/0f8HADMA9f8HAAMA0P8CAOz/+//z/wQAgv8OAA4Arf/p//v/AAAEABsA2/8dANz/FQAcAOP/HAA8ADQASwAdABkA1P8dAPb/HQCx/zoAGgADABIA+P8AACQA5P8FACMAUv/x/y4A7v90/woAAgD0//H/4//2//j/7//u/63/EwAiAAcAEwASABIABgArAPz/BgAEAPT/DgACACcACAAPAB8AIwDx//3//f/v/wUA4/8tABYA/v8fABkAwf/g/xgADQAMALD/+f/u/wQA3f/+/+f/AAD8/xcAyf8pAAAA9//9/wsAJQD4/6n/9v8QACkA2P/p/xwAFwBBAIL/KgD8/xIANADw//P/KACI/wEA1//e/xEAOgAQANP/5v/8//7/7v8tAO//+/+u/ykAFQAFAA4A8/8CACUAEAAKACMAIgDD/+T//f9bABgACgARAAIAFgBLAOT/uv/0//H/FwDz//H/DQArABcA/v/n/9D/HwDd/xIACQDu//r/7P/q/xcAFgDb/yYABgBY//T/AADH/xsA6P8GAAwAwP8OACIAAAD4/xMA+//v/xkA6/8fACUAHADz/+v/OQDz/wUAIADq/xIAzf8LAAkABADy/+//6f8DABsAIgDo/xYABAAPANH/Tf/8//H/DwAYAO7/FgC9//f/FAACAAwA4f/7/6b/HADr//T/LQBd/woACwDv/xcA6P9rANb/DQAUAAAADAC0/9b/6v8RAOf/GwDL/ywA6v/h/xoA9/8XAPz/IgAXAN//8f8CAA8A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HwDx//X/6f/u/xgAz//q/woAwv8XAAgA3f/M/+n/CQDi/0v/6P8YAKX+L//D/6f/9f8jAGH/AADo/xcALADh/63/DwAaAPT/2P/T/wkA8v8fABwA8f/j/w0ACABsABEA+//1/wgAHADz/wMA7//x/zoAF/8RAP3/2P+S//P/x/8sAPj/CwARAP//8P8wAAoAaP8OAL//CgD9////+/8fAMj/yP/T/+r/HwD0/wMAEQDq/w0AGwDl/wkABADU/+b/EADT//j/AQCR//r/tv8IAPr/CgDv/87/4P/6//D/GAA6APT/6P8GAOj//v8xAAMACADf/+n/JABJ//3/5//2/wUABgDk/xQAHADp/+7/EwDF//7/FAAZAMb/U//r/wsACADw/+T/JgCE/wkAAwAAAPv/RAAKAJf/HwDZ//T/MwAdAAsANgDt/wIAEwAAADoAAwAIAPT/AgDT//T//f8BANj/DgDd/+7/1v/j/zYABAD3/w8ALgAQABYA6f8JAAgA+/8NANP/9P8BABIAAwAZAPn/GgAKAOD/RQBb/x0A7//B/zj/RgDl/1IA8//k/93/1v8AAOv/d/////3/GQArACYADADx/woA8P8bAPn/9f8TACIA8v/n////IwDt/+3/+f/t/+7/+//V/+r/EQDw/+X/GgDX/wQA+//w/+7/zP/W/7T/7P/y/+D/AgDQ/+7/CAC0/w8ABwDt//r/5f8WAOL/nv8AABMA8v+g/83/CwDy//f////g//n/DQApAO//uv8QAKP/9//d/8//9P8dAPD/IQDy/+z//P/+/3IA8f/r//X/+/8XAPL/+v/x/+7/OgAaAAsACAAQAJP/+//x/wAA9v8QAAcA4f8OABQAFACC/x0AFv/+//3/xv/x/w4AxP8CAM//2/8bAP7/xv/P//j/yP4hAPL/DQD5/8z/JQCB/9P/7v/s/5X/CwDF/wEAAgAVAP//+P/j//L/6//5/8//7/8DALH/4f8kANv/CgD1/+7/7P8tAB4A7P/v/+L////l//z/+v8fAO7/AQATAN//FADw//f/1v9C/wYACQDk/wYA6/8dALT/FgD0/wEAJgCR/9z/mP8lAOb/BwDN/wkAAwBt/+X/DADp/zQA8//s/+v/BAAWAPj/m//4//f/5/8IAP7/1//b/x4A4//o/wcADwABAPf/GwD0/+7/BwAGAA4A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+f8DADQACwAHAAoACgAEAAEAFQDy//z/BwACAAgA/P/0//z/BQAGAAgAGACL/wkA///0/1f/8/8dAAAAyP8OAAwA+P/p//3/5f8AAPf/AwAfAPr+7v8CAAsAGwDq/wwA7/8JAPv/EQDs/wsA9f/b/hwA5f8aAA0ADgDt/wMA+//9//n/9v/1/xEAEACd/4z/3/8VAOn/IAD8/wIAEAAGAO3/AwAPACMAiv/5//n/EAD5/+//AgD3//X/hv8KABIA5v8FANT/8//k/+v/DQDj//b/JgAJAAcABQACAB4AAgABAAsADQAZAOT/BQD1/w0AEgABAAsA/f/5//3//f/+/8b/9////wUABAAEAPH/+//5/7v/+v8EAAwA8/8EAPv/7f8TAP3/9/8TABAACADx//z//P8HAAgAAQABAO7/MAD4/wAA9P/3//b/AwADAA4A///W/vn//v/s/wgA2v/6/+n/BgAAAPr/CQAYAO3/+P8LABQA/f8RAPv/BgDc/xIADAAGAPb/CgD+//3/GAAKAPz/AQD2/xYACwATAPb//P8GAAYA/P8AAAAADwAHAAMACAAKAPz////r/wsADgD///H/9v/9//T/EQD0/+b/JQAeAAMA/P8YAAwACQAGAA8A//8SAAYA4f/9/x0ABwDr//n/CQAIAPj/EwACABIA/f8EAAAAcQAKAP//9v///wEACAAaAPX/EAADAPX/AwD7//n/AAAEAAwABwARAND//f/w/+//yf/7/xMA/f/c//n/BgABANz/DgASAAYA8/8AABQAsv7w/yIACQAVAO7/CwDu/wAA//8DAPH/DwD2/27/FADc/xUAAQALAAcABgARAPz/AAD4/+f/CwDn/5z/w//n/xMA+P8RAP7//v8MAAMA+P8FAAsAJQCE//////8RAPb/9P8IAMv/AAAR/xcABwDr/w8ADwAGAOb//v8MANT/9/8mAP//CAD6//n/CgAVABEACgADAAwA7f/8////CwAOAP3/BwACAAEAAgD0/+7/if/9//n/+/8OAPr/9v8IAP3/IgD3/wkAAwD///j/EQDx/w8AAwDz/xAAEQAQAO3/9//0//v/DAAcAPb/+f8nAPT/BgDx//T//f8JAAwADgAAABsACwD2/4j/BwC4//r/8f8KAA8A9/8MAB0A9v/2/wgAAQAAAAIA+v8JALT///8FAAEA3/8DAPT/EAARAAwA9P8HAPH/5f8PAA8A+//3//3/BgACAP7/DAAAAAEAAAAFAPv/AwAAAOH/BgAcAO3/DADl//z/AAAeAP//6/8jABwABQD+/xsACQAJAAUACwD8/wYACgD5/w0AGwANAO///f8GAAQA+v/5/w0ACgD7/wIA/v/v/wkACgAQAAgAAQACAAQA8v8GAAEA7/8EAPv/+//8/w0A+v8IABcAHAAFAOn/8/+9//v/GQAAAPT/AAD2/wcA2P8VAAgABwD3////AADp//H/MwALABsA7v8JAPD/AAD///r/8f8CAPX/4P8RANr/FwACAAwADAACAB4A8f8BAPn/7/8JAM3/pP+//+L/GgDu/w8A///3/wAAAgAOAP//CAAZAAMA8//5/xMA+P/2/woA/P8fAED/CQAMAPb/EgAaABgA6//x/wwA9v/2/xQAw/8TAOj//P8GAAwACQABAAAABwDv/wIADQAKABIAAAADAAcA+P/+//j//P+d/zEA9/8EAAYA9v/+/wcA8P8bAPb/+f8QAP7//P8DAPT/DwAEAOv/CwD3/xAA9P/+//b/+v8JAAwAAAABAB0A/v8CAP7/AADy//z/EAARAPz/HgAHAP7/R/8EAJr/8P/w/wUACAD8/woAFwDl//3////2/wIACgAMAAcA3v/1/wkACADK/wAA+v8DAAsABgD5/wEA+f/g/xcAGAD8/wMA/f8IAPj/AQACAPf/+P8CAPj/+f/1//b/7f8EABUAxP8GAG///P8CAAYABwDs/ysAIQAAAPz/JQD3/wwAAQASAAYABgATAAEAAQAZAAoA5f8FABIACgD8//H/+f8HAAMADAACANz/AgAFABMAAwACAPj/BgD1/wYABwDo/wEA+P8CAO//+v8MAAoAAAAfAAYA5v///+j/AgAbAPv/CADv//n/JwDY/xkADAACAAAABwD9/wsAAAAwAAIABwD7/wAA8f8AAAgAAADy/wUABwDt/wUA7P8NAAYACwAUAPr/HgD4/wIA+v/h/wUAA/+X//T/6f8FAPv/+f8DAPP/+/8BABsAAAAEAIj/OwAcABUABQD6////DAD8/x0AQv8EAP//6v8KABQAGADx/woADAACAPj/AwDA/gkA3P8AAOj/JwAAAAQADwAIAPP/9f/1/w4ADQD8/wIABwDy//7/8f/n/8b/1//6/wYA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AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA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JQAIAAEABwAOAN7/EQBtAN7/w/9W/wMA2P4FABgA8/8MALkACQAKAAEAEgDw/wYA+f///wEABADg/wsA+P8IAOr/7v8CAPP/AAAnACIA+/8HAAoACAD2/wAAFgAWAC8A7//9/xkAJgALAAAAGwAJACcA9v/4/9r/JwDH/yUACAAEABUADAApAC0A9//m/9L/DQDd/wcAHAANABYACgAOACsAFQABACUA8P/t/9X/FADs/wEACQAeABIA///5/wsAIwDy/xEA6v8TAPT////7/wQA/v8aAKv/XQAKABYAEgAsAM3/BwAEABcAMQAgAAMA8v8ZACMA8//2/+7/cQCp/wAA8v8JAA8Atf/4/ywA+/9VAPT/EgAbAA0AAADa/9T/EQAcABgAAf8DABUA6f/l/wAA0v/7/wYACACU//z/EQAMAPL/DgCZ/y8AIwAf/wIAAQAAACIA5f8UADoACwAPACkABgAQAN//+/8zABMACwAQAAcACgD2/+D/5P8pABIAAAAJAPv//f///wwACQAUABEA/P/0/4H/AgAeABgACQDc//X/1//w/xIAGQDg/73/5/8GAPj/8v/O/wAA8/8BANX/pP8gAOb/+/+wAPr/Rf8KABQACAAUAAcACAACAAIA8v80AA0AAAACAN7/LwAwAOn/+/8wAAIABwAAAAMAGwA2AAUA8/8RAAgAuf8OAKL+5/9wAK//6f/T/uf/EADu/wgA+f8MABoABAASAAsABAACAP//AAALAOP/DQABAA8A2f8IAPv/BAD9/xEALAAOAAgABAAGANz/BgAQAP7/IAD9//T/CAAxAAsA+P8YADMAEwABAPr/rf8pANz/CAAIAAAAEwD+/xAAFwDx/x8A0/8DANj/BgAUAAMAHAAIABUALwD7/woAHQDc/+f/tv8mAOf/DwDn/+r/BgAQAAEA7v8VAPv/BgDL/yAADQAAAPb/BwAEACEACgBQAOz/CAATABwAhwAJAN7/GgAoACAAEwDk/xUAUQASADEA5//Y/yMA/P8qAAIAyv+k/xEA9f/Z/ycA9P8PAB0A7v8LANj/7/8YABMAFgAN////DgC9/wkAAADa//z/BgALAJf//v8jAPb/2P8KAAQA7/86AO3/6P8TAAgAFgDw//b/MwAFABIALQDo/wsAn//9/z8AJgATAAgADQAHAOf/+P9dAAwAAwAZAP//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5f/L/xYAKQD8/+T//v8fAA0ALQAsALH/IgAgADcA5v/b//3/+//8/9n/8/8FABUADwD7/wIA8v/6/xsACAD9/ygAnv8EABMAxv8EAAIACwD7/xcABQAZAB0ADQARABQAGADw/xUACwAGABMA5P/0//T/FgCV/wAA8v///4b/EAAWAOz/DADv//L/CADn/xYA1f/b/zUADAAQANP/DAARAMn/5/8LADIAAgAWAAoAOP/3/xwABADq/wUAAwD8/xsAFQD3/zUAAAAAAPD/DwDJ/wwAFAAdAAwADwAZABAANwAtAAcABQAaADYAAAD9//j/uQAJAAsAAwABAAcA/v8KAAUAMwDO/3b/8f81AOH/FQD7/wAADADR/xgAK/8LAFQA2//E/+//AQAIAOv/x/8AABMALQDH/7T/IwAHAB8ABACXAAcA8//2/wMACQD4/4v/AgDZ/+7/DAAEAAkAGADx//j/IAD3/+X/LQDz/0sAj//f/wgANwABAAcAGQDt////IgAIAAMA5f8TANH/Af/9/xkA3/8OAAEAHwAyAEcA/v/f/x//z/4WAO3/BgDQ/wEAsgASAAUAGwALAPX/EgAMAC0AGQBLAB0ABwDo/xIAAwDu//z/0v/H/+r/JgD3////3P84APv//f8MAPr/DQCl//3/HgD7/woA/P8QADgA1//f/+X/AQAfAAkAQgDi/xAABQAiACUA6f8sAAsAKQDw/6//AADr/wAA5f/0/w8AGwAAAPb/BwAVAAQAAgACAPn/CgDq//7/DABPAPv/FAABAPn/GgD+//X//f8FABcA9f8TACgA7P/3/xAABAApACAA+P8DADwACQD5/wgAf/8FAB0A/f8oAPT/BwACAOX/DgDW/7n/HgCF/xMABwAEAAUA6f8VAAkAIAD//wgAEACB/wwAKQD4/wQAAAABAPj/HwABANv/IQAAABIA5P8VAO////8EABIADQAJABIABQA+ABgACAADAEIABgDp//T/BwBl//3/DwABAB4ADAD6/wgACgDv/yIAGgDx/yYACQAKAAQABAAUAOP/CQA9AAwASgDn/8r/6f8FAAAAEgDR/wAA/v+7/tX/wP8YAAAADQAKAEX/FgAIAPv/AwARAAsA1v///+H/7f8HAAMA/v8YAPT/DQANAKD/xf8pANP/KQAvAAsAEwAvAAkA/v8MAO7/1f8GAAUA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DQAAAO3/p/9pABIA7f9i/wIAFAD//wIA1f8UAIwAGwANABQAFgDv/xUABwAlAPT/JwAAAAUA+f8FAAYACgAHAAUAAAANAA8A8v/6/6b/FwABAPf/EgAHAA8Aw/8XABsAAwDw/wMA//83ANz/9/8AAAMAEQAJAMT/CgAFAAAAHwAKAAEALAAHABkA9P+L/wAA+P8HAPH/BAAMACEACQAGAAgACAD+/xwABgAAAPH/1P8FAAEAAAAAAAoA7/8HAAsAAAD6//3/CAAOABIADwArAPT/BgACAAcABAAPAPz/EAA6AAAA/v8AALD//f8FAPL/CQD0/wAADQDk/xwA8v+s/xoA4v4WAP//AQAIAOX/BwATACAACAAEABEAkf8MAAYA/P8FAP//CQD+/wgAAgDw/xQABwAEAOX/AgDv/wEA//8HAA4ABwALAA4AOgARAAcACQAGACUACgAAAPn/7/8KAAcABwALAA4A+v8CAAAAAAAVAOD+/v8hAAQABwD//wYACwAAAAUAPgARACUACQDd/+j///8HAAMA5f8AAAkAa//V/9H/FgD4/yEAAADk/w4ACQAAAAUAGgAFAPj//P/1/+v/BAAEAAAADgD//wgACgAS/8P/JwDo/wsAFgAAAAwAEAAMAAMACAD0//X////7/wAA9/8XAPn/xv/o/xAACwAQAPr/iQAWAEYACQDe/3kAGgAFAP3/DwDY/xAAFQATAA8AEAD7//v/nv8AAB0ADwAmAAMA9//3/wsADAAVAAcA8//7/wIABgAGAPb/XP8CAAcABgD+//j/BgCu/xsABQAGAAsABQAKACAA9f/e/wsAHgDu/w4ABAD6/wAAEQArABoAAAD8/wYA9f/3/1v/AgD8/wIADgDn/xQA+P/8/xUAEwDn/w0AGAARAOL////4/wwACwAIADwACgD6/w8ACwABAAUA///8/woACwAJAAoAMQAJAPr/Mv/0//b/TwD//wIA/f8MAPn/1f/3/47++v8dAPf/AAAZABkAEgD0/9v/AgABABYAFAABABsA7//6/wkAFAACAP//DACm/wIA+P8KAAAAAAAMAC4ABQArAPT//P+F/wcA9f8LABAAAQD+//z//P8LAAAACAAuABEA1f8RAAcAAADJ//H//v8MAAgABQANAP//CgD6/wwA/v8GAP3/BwABAPH/GgANAA4A0f8QAPz/6P/r/wIAAAADAPH/6f8LAAoA9//h/w4AAwD9/9H/0/8CAP7/FwD5//n/b/8KAAwABQATAA0AAQD4/wIA6v8EAAgA///2/8X/FQALAA4ABwAjAPz//f8CAPr/DgAIAAwACgALABQADwAFAAgA4//4/2//BgDQ/wkADAAdAAMA7P///////f8GAPX/AAD4/wAABAALAOn/CQD4/wwA5P8GAP3/FgABAAwAHQACAP7/AAAJAPP/CQAAABkA9f/z////EwDs/g0A/v8HAP//AgAKAPP/0f8OAM3/FgAGAAcACAAEAKv/DgD3//T/FwAAAPP/EAD9//L/AAARAB0AIwADAP3//f/7//P/r//6/wAABgDn/wUAAwAKAPr/AAAAAPn/DgD4/wAAmv/7/wEAJQABAAIA7f7/////CAASAP//AAAAAAgAEAAeAA8ADAA0AA4AOQANAPH/8v8HACMAAAD+/wUA9v/q//b/8//9/wcA9v8CABAADgAMAOX/zP8FAAAAGAAW/xwADADu////8/8KAA8AAQAIAKb/AgD9/wwACAAAAH//3P8FAPr/+v8BAAcABgDy/w4ADQD//wAABwAEABAA9v8NADIAEwD2/wwABQACAA4A4//y/wkACQAAAA0AAAALAAgAHwACAPz/CQAAAPz/4/8HAAMADwAFAA4A9//0/+f/CwD4/wcA6P/i/wEAAwAeAOn/DAADAPn/zf++/wwACQATABUA+/+B/wYAAAD6/yoA9v/4//f/AgDq/xQADAAFAPD/sv8PABIAEQAGACcA/P/7/wEA+/8aAAEACwD7/wsABADh/xEA4f/i//n/nf/6/9f/CAAMABUAAAD7////BwD5/wMA+P8AAPf/AAAJAAcA5v8CAPr/EgDV/wgABgATAP//AwAjAPr/+/+2/wYA9v8JAJ7/DACQAOz///+l/xkA///+//7//P8AABQAAACU/wEA3P8EAAAABAACAAYAFQAeAPv/GgATAAAA7/8LAPf/9P/5/w0AIAA1AAAA+f8uAPv/9P+P/3r/8f8LAOT/5P8BABMA//+2/7X+BQAQAP//DAD2//r/7//z/wgAAAAnABoA8f8WABAACAD5/wUAAAAQAAIACQAJAB0A0v4UAAwAAQDy/yIA5/8IAPz/AwDz/+P/+//6//D/CwDy////EQD1/wIA6v/k/wUABQAYAAz/BAAKAOn/AAALAAEA8/8BAAAAif8AAAAABAD3/+P/CAAMAAwADwDq/wMAGAD7//X/DgAYAPr/CQD6/xUAEwD8/wMAMwASAAwAO/8GAAQAFgAIAGsAAAAKAPf/CwADAPz//f9FAAwAQwAGAP//z//q//j/GAAOAAQA+v/6/+j/3/8HAPP/FgDp/97/JABdAO3/6f8KAAAA/v/I/9T/DgAIABEA9////5//+/8ZAP3/MwD9////9P8MAOz/7P8GAPX/+//g/ygAev8HAAUAKAD+//n/AQD5/xgAAQANAPv/DQAAAPv/DAAEANb/+v9+////1P8AAAkAEgACAAIA/f/+//r/AADy/////P///wQACADk/xsA+/8PANX/EgD7/wsAAADH/yQA+v8AAPz/BAD7/woA9/8GAOv/6f/v/zgAHADu/wAABQD5/wsAAAAIANH/CADv/w8ADwCpAAcAAwDR/w0A+v8AABIA8P8AABAA/P8FADQACwAiADsAAgD9/7//BQDx/1//5P/j/wIA0P/8/wIAEAD6/8H/8P8JAA0A/P/0/wUA8P8JAPv/2v/8/+v/9f/s/wIADwD9/wEAEgABABcA/P8LAA0AEwAkADIABADw//7/8v8QAAoA9P/9//L/zP8UAP3/7//+/+7/8/8OAAgA/P/l/9D///8CABUAE//w//z/7P/v/w0AEACz/xr/DwCf//r/+f/7/+7/8v8OAO3/JP/4/+P/BgANAPr/8f/3/yQAAAAAAPP/AwATAPr/BAA5ABoABQA1APz/+P8HAAUA6v4DAKL/u/8KAPz/CwAyACoAMADz/wgA+v/9//j/8P/f/wsAAwAHAP3/6//4/wAA6v8AAOj/4//x/woAGQDw/wQABQAAALv/wP8SAPf/EgAAAA4A2//o//r/mf80ABAA+f/2/xEA6/8TAAgAKwAIAPz/JwAAAAoABQAoAPj/9P/5/+//FAALAP7/7//v//3//f/w/+3/6v/7/5H/+v/c//3//v8OAAQA///+//z//v8CAPL////5/wIAAwAOAOT/9f/2/xoA5/8LAPr/CQAAAAUAJwAEAAIACAADAP7/6f8BAAYA+P/h//b/mP8AAN///f/7//7/2/9AAA8AAQDh//H//v8QAAQACADt/woAHAD3/wIAEAD2/w4ACgDy/ywA0/8NACMAAwDx//j/JAAHAPD/iP9Q/9z/AgD2/wAA/f8eAAEAyv8GAAwAAQAAAAcA//8DAA8A+////wEACgAAANf/AwAQAHD/9P8tAP3/GQAMAAkABwD2/1L/BQAJALf//P///wAAAADx//b/7v/O/0UA+v/u/wAA7P/z/woACAAJAO7/x/8KAAMAFAAW/wsA/v/q/+j/DQAPAOv//f8LAJz/+f8AAPP/AQD3/wsAAgADAPT/8/8YAAoA+P/s/xsAKgDf/wMA9P8QABEA+//y/0AABAAJACb/+P8TAA0ACgBHAPf/OQD2//T/c/8OAOj/EQAUACoA8f/0/wQAAAAAAAcAAAACAAMA+//3/5P/DwDr//n/6v/f/wkADAD0/+z/FAAPAAIAuv+6/wwAAQANAAAAGQD3/5//AAD8/zIACgD+//X/BgDm//n////6/g8AAAAYAAcADQAEADAAAgD0/wEA+v8PABAACwD7/4/////9/xQA/P////z/CwD2/9b/9v8BAAsACgD//wEA9P8FAAQA6f8BAPL/CP/+/woA3/8iAPH/CAD9/z3/7P8AAAkAFAAnAOH/HQD7/wMA/f/U/woAAAAAAAIA9v8XAP3/6P/y//v/8P8KAA3/DAAIAA0A5f8GAAwABAD+/7v/CgAhANr/+v8TAPv/FADx//P/MAAhAAMAIQD6//L/+//4/wcA8P+o//P/+P8CAAgA/v8CACIAAAD3//7/EAD8/+n/8P/+//D/EgD4/wEABQADAAEA7//3/w4AAgDw/zUA+v8UAAkACABz//L/DgD7/wkA7P8AAPj/CwA3AO3/8v/5/7r/JgD1//H/BgD1//b/CAATAAoA4/+r/wAABAARABP/8/8FAO3/9v8KAA8AEAAJABEAnf8DAAkA+//8//7/BwD7//3/9//u/w4ABgD//+L/BAAVAP7//f/5/wMADwD///r/PgAVAAQABgAFAOL/DgAEAPv//f9Z/w4ACAD7/wkACQAFAAQA6v+3/+3/CAAPAO7/AAD4/wAADAD9/w4AKAD6/+f//P/p/+L/AAACAAUA7v8IAAQA+P/C/7z/DwDn/xAA+/94//7/+/8GAAAAMwALAPv/9P8FAOr/AAB7/x4ADgD//wAABgAAAAcALgAEAPb////n/wkACgAJAAYA/P/+/wEA+v/4/wQA6v8UAPj/2v/+//3//v8SAPv////6/xsACgDk/wEA+//8/wAACgDY//3/CQAiAA8AOQALAP//FAABACUA1wAfAP3/CAAFAPv/DgAFAAoAEwDz/wUAAADs//n/BgACAP7/KAARAAUA6//e/wwAFAABAOv/+P8GAB8A9v8AABIABgAPAAwAtv8vAPn/5/8lAAAA/f/m/wEAEQDz/8j/AgD//wcA9f8AAAUAJQADAA8A/P8VAPj/7f8FAAAA/f+t/wAABwD3/wMACgDf//7/EgAIAPT/FwD9/xIAQgANAEkA+/8LAP//BQAOABEA+P8NAM3++v8PAPv/wv////b/+v8RAPb/LAAQAPT/BADr/8//7/8DABYAIf/9/wwA8//U/woAEgAJAPv/AwCj/wMA+v/8/wcADAAHAAAA/P/5/wkAdgALAAAA6P88ADEAAAD9/wEAAgAOAAcABwBAAAkACQAVABn/HAD1/wYA/P8FAA0AEAAFAAQACgDy/wAA/v8AAML/4/8IACAAAAAJAPf/AwALANz/EQB0/woA8//f/+f/3/8BAAsA/P/q/w0AGgD7/8r/uv8PAOj/FgD5/1kACAD1/wIAAgAhAAoA+//+/+n/7/8FAPr/7/8SAP3/AwAAANP/2P8wAAoA/v9o/wIABAAAAAkA//8JAPj/AAAGAPv/AAD8/x4AAQDT/93/DQD8/xMA+//1//X/RQALAOX/t//0/wEADQAIANf/LgASABoAFgBU//f//P8TAAoAJgDf/xAA/f/8/wgABwALAAsADAAXAP7/DwADAPr/5P/X//D/AQDq/xUABwADAND/AAAQAAQA5v8IAAIAEwD8//7/DAAGAAcACQAEABMA//8EACsABQAKAA8AAwAOAPf/af////7/CgD8/w0ABwAjAAQABwADABAA+v8NAAUAAwD4//H/BgAHAAIABAAFAOn///8NAPv/9P8BAAQAEAAWAAsAGv/s/wEAAwAHAAUABQD5/w4AMgAAAP///f/F//j/AAD9/xYA+P8EAA0A7f8KAOb/rv////r+FAADAPf/AwDu/+z/DgASAAoA/f8QAJz/FwD9//n/CwADAAgAAAADAP7/+v8CAAwADQDp//v/BgD8////BwD5/wcABAAPADwAFwAIAAUAMQD+/woACgAAAAAAAwANAAkAAgAQAP3/AAD6//z/FgD4/wgAHgANAAYA/v8HAAsA+/8JAD4AAwD9////4P/l/wYA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if/x/xQAAQDx/+//5v/6/+H/LQANAP//CgABAAQA8/8sAAsA9/8HAPH/AAD1////AgDq/wUA9P/j//r/CADw/w0A+v/W//H/EAAZAB8ABQAEAOz/DQAv/+j/+v/7////EAAFANb/AwD0/zcAFAD9/wwACQAOAPv/KAD0/wsA/P8DAAgA1f8MAAgABAAsAPj/AAAAAOL/8f///zQA//8rADoADAD6/9f/AwAMAAMAPwDt/wcAEwDv/wAAEgD6/wAAAQDy/xQA/f/v/iYA8v///+z/+/8DAPb/xv/+/wIAFgD7/wYAAAApAAIAEQACAAEA8f/3/wsABgDz/xUAAQD//wAADgADAOv//P8PAAwAAwAKAP//DgD7//P/AwD7//z//P8FAP7/EQACAAkAWP/2//D++/+8/wMAAgADAB4A+f+EAAcAAwAvAOz/zP/P//n/EgB+//3/+P/y//b/BAASAAIA9/8bAKj/8/8CAJj/AAALAAQABQD8//z/DgASAAcA/v/g//v/IgD///n/+/8RAOX/BQDL/0MABAAFAAQA7v8BAAEACAABAAoA+f8IAAIA+v8BAPT/AQAAAP//+f/5/xAAIQDy//z/2v8HAAoA5P8hAI3//f/c/wcA5P/l/wIACAADAOL/AwAMAPT/0v+9/xAA8v8JAP//LwAEAAcAAgAGAPL/DgAEAO//0//l/wMA1v/2/xAAAAAWAAAA3P/W/yoACgAAAMP+/v8IAPb/AAD8/wMAAAAAADUA/v8FAPr/EwAJANH/6P81ABQAIAAHAAwA+P8YAAP/4f/3/+3/9f/+/wcA3//v//b/BgAWAOv/9v8EAAMAEwArAAQACwD8//P/AwDz/w4ADQAFAB8A/f8DAAEAAADo//X/8////+P/FwAEAAIAyP8IAAcABQASABEAAQALAAUA9f8QAAEAAgAGAPf/CQD7/8P/LwADAAgA/////wIA/f98//3//f8MAPv/DQACAAoABQAMAAEAEgDu/wYABwADAPn/GQAFAAgABwAEAAUA7P///wkAAAD0/wEABQARAAMABgDs/+7/AgABAAoAAgAKAP//DAA9APv/EwD//7b/8f8AAPb/EQD6/yEACwDs/wEA3/+5//v/K/8TAAIA+v8AAOT/IAAGABAACQD7/wMAkv83AAAA+P8NAAcABQACAAEA///0/xAADAAHAOr/AAAPAP//+//9/wUABgABAOn/OAAJAAkABAAFAPz/BAALAP7/AAD7/w4AAwD9/wkA+P8EAPz/+P8VAAUABwAQAA8AAQD5/wYADQD2/wsALgABAOv/EgDQ/+r/AgAGAAUA5P8LAAQA0//S/7H/BQD0/wgAAAAOAAAACAD9/wEA3v8HABIA9f/3//H/BwACAAAAEgACAA4ABAC3/6L/IwADAAQAFQD3/+7//f8DAAEABQD8//z/DAD6/wcA7v/u/xAAyP/0/x4AFQAKAAMADwAFABQA8P/l/xUAAAD7//r/EQDg/wkAAQAOAP///v8RAP3/8v/+/x4ABAATAP//swAAAAEADwAeAAYA///+/wQAAwAFAPb/0f8PAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA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BADg/+T/CgD9/woAGQD//wwABADj//f/FAAWAN3//f/7/wEAFADV/wgA8P8GAAAAEgABAAcA7//t/w4ABgAGAFP/9/83APX/qv8mAP3//P/6/wMADADZ/5H/9/+3/xEACQD//xAASADc/woA+v/c//H/5P8UAPX/BAD3////DwAQAPb/BgAQABwAHAD//wEADwD5/yEAEwACABcAAgAEAPb/BQD4/xIA8v8UAOX/9v8CAPP/bv/v/+X/3f/t/+3/AAARAN//JgDK/wT/EgAjAD0APAD9/xcAyv/R/xsA//8DAAEACwDU/x4AFgDk//f/BwAGAPr/6P8BAOf/BQADANz/x//n/x8ABQD2/+//DQAWAAUAwP76/woACAD5/xQAWgDX//3/+//k/xMA+v/h/+v/GAAOAA8ACAD4/xkAIADg/yMA6v8EAPn/AAAeAPT/HwA//xEA7v/2/+f/PQAKAAwABQDC/9//DgDr/9n/6v81AAAA4v/4/wQA+P/k/xEAEQDU/x8A/P8BAAwAn////wcA4/8kAOv/AQANAAIABgC9/+L/9f/M//f/IADo/xgA//8IAOz/AwD+//P/5v/5/3T/8v+t//T/HQAeAB0A4P/2//f/DQAQAOj/CADt/0kAIgANAOL/FADy//r/HAC7//3/+P8rAA8AWgDj/wAA+//5/+7/BAACAP//EAAbAPf/DAAIAOT/9P81AOb/BgAEAPL///8LANv/CwAFABEAFAD+/w0ABgD1/9b/HwAOAA4Aw//z/xMA+v/h/h8AAgAHAFMA/P8SAN7/lf/k/8n/FAD5/xIAFABAAOH/GwD5/w4A///z/wUAAQDz/wkADAAIAPz/8/8EAAoABwAgAPj///8PAPr/EAAdACMA1f/j/w0A9f8BAOn/DADn/w8A/f/y/w0A8f9///H/4f/A//L/6////xMA/P8WAMv/Av8OAPL/NwAsAOP/GgDI//3/KAADABEAAAAJAM7/HgAWAPn/BAASABIA9//u/wIA7//i/woA4v+///3/DgAPAAEA6f8FAAkABgAOAPf/CQAQAAUAIADv//n/AADy//X/8f8FABEA9v8bAPb/CgAGAO3/FQAGAPr/IAD+/w0ABwAEABoA8v8TAET////4/xcA4/9AAAgAFADt/7n/4f8mAO//3f/w/ywA8P/f//j/DQD6//r/EQAQANT/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2/+h/wAAHwDg/wAAJwAfAA0ADQDy/xQAFAAGAAAA/v85ANT/EQD+/woA8/8KABwAAAAYAAcAEgARAAMAMgAmAO//zf++/xgALAAIABYAEwAaACwABAAsAAwAvP8HABAAEQDi/4v//v8MAA4Atv/3/2IABQAKAPr/AAASAAAAHAAYAKr/HgAjABMASQB7//f/HQADADsAz/8yAAUA8v/6/wIAtv/w/zgA2/9tAAYAHgAOABAAw/8JABIA/v8DAAEA5f/S/3r/AwAJAB0AMACV////JQAVAAsA4//s/y8ACAAgAC4A1v8cACYACgAtAAEAFgDx/zkAFACNAOH////u//n/vf8LABIA6P8gAAAA7f///w8AGgD7/yQAxv///w0A5P///xwApv8ZAAcACgAcABsAHwAwAPr/6v8CAAkAHAAbAFkAEwASAOn/v/8ZAAkASAD1/xYAxv/v//b/0f8jAAUA6/8VAIgAwf8AAAEA9P/v/wYAJADp/+v/FwAKABUA9P/r/xYAGwAsACQAAwD//xYA7f8EADQASQDg//r/DAABAP//HQAiAP7/GgAIAPH/9f/y/6r/3f/4/43/+f/j//z/+f/q/wUAlP+l/xIARABuAMz/7P8XAKX/zf8fACQAEAASAAQAJQAuACIABADv/xsABgAMAAEAIwDn/w4A3P/J/6T/4/82AOn/AAAcABcAEgASAAsAFQAaAA4AAgDM/yoA6v8YAP3/AwDl/xYAGgAMABsABgARABIA+P8xAEEA6//W/9b/IAA4AAAAEAAHACkAMQAKACMAFwDA/wUADgAUAPD/iP8NACIAOAC5//z/YgD5/wwA/P8CABQACwApACIAu/8YABkABwBBAHv/9P8VAO//OgDO/yQACADG/wYABADx/+3/GADL/2sA9/8jAA8AFQDH/wwAGQD4/wUA7//l/9z/gP8cABgAGgAuAI//8/8bAAkACQDR/9H/+v8RABoAJQDY/yAADQAIACwAFQA0AO//TwAHAI8A9v/2/+//AwC2/wcADgDf/yAA+v/f////EAAcAAUACgDB/wsA+f/l//j/DACt/yEA7/8DACAADgAnACoA5//q/w4ABgAEABkA7v/w//H/AgDB/9j/CQD5//P/0//H/+v/+P/L/xYACADs/xMAjADO/x8AEgDz/yQA4v8pABMA6f8SAP//FgAFAPH/BwASACEAJQAAAPj/GAAAAAgAHQBJAA0A9/8NAPv/pf/t/8r/IwALAPX/9f8BAPP/r//e//n/mf/8/9j/4v/2/+7/GgCK/6X/FwAbAHIA2/8DACEAqf/s/ycAJgAQAPX/DQAeACAAHAAMAAIAHQDq/wMA/P8UAPr/8f+U/9j/qf/8/x8A8f/l/+r/GgAYAAMABwAWABoANQAAAAAA6f/g/wkA/P8TAAgAHQAdAPL/IwABAAwAGAAAACMAFgDn/7P/9v8IAEYAEwAcANX/EQAoAA4AAAApALP/AwAUAAQA2f+X/wAAGADw/7b/EABdAPT/CgAJAPv/KQAYABUAIQDG/xwA7P8DAP//cP8CABIA9v8bAMH/FQAAAPz/BAAHAKH/9/8TAP//awAIACcAFgAYAKr///8jAPj/xP/W/+L/5f99/xIAGQAcACYA7P/3/woAHgAKAM7/+P/p/wcAHQAgAN3/GwDp/w4ALQAAAC0AAwAMABIAhwD9//n/9P8IANX/AgAEAOD/zv/y/+r/4v9SABwA+f8DAOz/BADx/9r/9/8OAKv/GgDr//3/JQAIAEsAGwD0//b/CADp/wcACwDw/+H/9f8DAMn/6f8NAP7/8P/N/8T/5P/6/9T/IAD3/9D/JgCJANL/GAAlAPj/IADe/ycACADp/ywA+/8hAA0A+/8JACwAMQAkAAMACAAYAPf/DAARAEYAEAD1/xAA2/8LAPX/0P9HAPH/8//1//j/+v+v//D//v+T//b/3v/r/+3/1f8XAJD/pP8hAA8AcgDa/+z/GgCo//b/dv8zABQA+P8PABoAIQAWAAwAAgAZAMz/MwAIABYA4//w/8b/4P+q//H/GADx/+b/AwAiABUAGQAKABoAJgAQAAIABwD4/+T/CQACABcAEAAWABgA+f8yAPr/FAAUAAgAIQAbAN//uP/a/wkARgAaABYA2P8ZACgAFQAAACAAtv8AAAoA+//Y/5H/6/8NAOH/tP8DAFwA8/8KABsA/v8rABcAFwAiAMr/JQDw/wsAAQBz//3/GAD1/wsAxP8sAAsA/f8FAAYAlP///wcA+v9rAA8AJAArABIAq//9/x8AKADE/9f/2//p/3z/CgAZABAAJACz//j/CAAcAAsA2f///+b/EwAdABMA2v8MAPH/GgAqAPz/KwAlAAoAGACIAAgA+f/n/wIAyP8HAAIA5/8PAOP/7P/o/zIAHwD3/wcA+f/v/wUA4//c/wYArv8oAN//9/8hAAkAQgAeAPr/6/8OAP7/BgAWAPL/7f/1//7/mv/X/xIA9P/p/+X/yf/p/xAA0f8aAP3/6v8kAI0A1v81ADMA8P8ZAOj/HgAWAO3/IQDi/wsACQD7//3/LQAbACIAAgD//xgA7/8RABcARQAQAOb/Df8uAAgA7//Y/wAAGwABAPv/8v/p/6f/6//2/4z/5f/f/+r/9f+w/xoAYv+h/yAAEQBzAK7/HQAXALL/7f8qACoAAwD//wkAGAAdACMADAAAABEA+f8EAA0A///y/wAAzP/l/6v/AwAYAO3/4/8ZACIAGQAeABYAFgArABcAEAAJAPj/3f8SAAMAEgASAAYAHgAAAID/CQAfACAA/P8kAA0A0//R/9b/MgBDABgAGQDS/xUAJwASAAAAFgC4/wcAHwAXAOD/l//7/x8A7/+x////XwD0/w4ADgAAACwACwAXAA4A0P82AO//DAAGAHL/+P8aAPn/HQDG/xgA6v/2/wcABACk/wMACgD9/28AEwAvAA0ADACs//z/EQD4/8f/5f/e//f/g/8IABsADgAOAKn/+f8HAB4ABwDW//L/6/8KABYAFwDc/xMA8P8IACsA+v8pAA8ABAD9/4oACwD4//L/AwDe/wwACQDi/wUA4v/l/5n/KQAQAOz/BADs/9//BwDq//r/DwCq/xYA7v93ABoABQA+AA4A8v/p/wsAEQD//xQA9f/m//H/AQCb//P/FAD+/wAA8f/C//T/GgDF/xkA8v/o/x4AjgDZ/ygAIgAIAB0A3P8oAAwA6v8rAPj/7/8GAPz/EwAcABkAJgD7/xIAFgD3/xYADwBBAAYA6//R//X/+v/t/+r//v8dAP7/+P/5//j/qv/p//b/l//b/+D/6f/7/8H/EACE/6T/FAAJAHIAmP/3/xkAsv/d/yIAIwAiAN//AgAZAB4AIAAJAAEADQD2/wIAPwATAO7/AADc/+f/pf/y/xQA8f/2/xcAGgAUAAwADgAXACMAFAD8/wMA8//e/xcAAwALAIP/CAAfAP3/LgD3/xYAIAAmACUAFgDi/9H/5/8NADkAGgAFAN//IQAvABAABAAwALb/BAAMAPH/2v+W/+3/DQD8/7j/AQBZAAoAFQAEAPz/JwAeAA8AQwDY/yIA9v8RABkAZv/t/xYA/v8tAMz/EgAJAPT/AgACAKr/BAAFAPj/bwALAIf/EAAJALr/CwANAPz/3f/l/+T/8/99/wYAIgAKABwAqf/6/xMAHAAGANX/+P/4/yQAGQAmANr/EQDy/xgALgAEABkABQALAA0AiwAGAAoA+P8HAND//P8GAOf/DADb//L/+f8bABQA+v8HAPP/R/8UAN//AwAHAKv/MAD9//z/JQATACcAMQDs/93/CgAAAAQAIAAAAPz/GgD+/67/CgAcAPP/9v8DAMn/7v8WAMb/JwABAOr/IwCJANX/OwAUABgADgDv/yIAAQDo/y8AAgANAAsA9v8MADYACQAmAPP/8P8bAP//FgAiAEIACADm/yH/AwAEAM7/9f/1/xcAAADw//f/+v+n////7/+v/9X/5f/p//X/xP8UAIf/n/8MABUAcACS/w0AGQCw/+j/IQArAPj/BAANABYAGAAoAP7/BQAcAPv/+/8HAAwA9P8UANj/2v+o/+n/GgAJAPf/GgALABIACwAIABcAJAARAPz//f/2/9L/FAAFAP//1P8TAIH/5/8mAAwAGwAYAAcAMQAiAOj/xv/4/xUAMQALAAkA6v8gACoAFwAFAB8Au/8DAAoA/v/b/5T/7v8vAAEAuv/5/1gACgAOAP//BQAeAPD/FwAdAMX/GwD0/xMAJwBu//v/CAD+/zgA0P8iAAoA7v8IAAAAqf8FABUAAABtAPb/LAATAAAAvf8KAAgA9f/s/+D/5f/q/37/DgAeABcAKgC2////GAAeAAEA4f/8/+n/IgAdAB4A2/8KAO3/AgAuAAAAIgAAAAgABgCRAA4ACwD7/wMAzP8jABEA6P8PAPT/7/8HABcAGwAAAAQA8v/f/wAA5P8OAAgApv8lAP//AAAfAAkAKAAkAOD/4P/8/wgABQB8/wAA9f/w//T/uv8LAAUA+//0/xMAyf/v//v/1P8eAAoA+f8eAIoA1f8XAAsA/v/5/9L/NAADAPH/HAACABEAEQD0/xYAQgDu/yYA+/8GAA8A+P8UABUANgD3//P/EwD4//z/7/8RAPD/FAAMAPH/4//4/7L/4v/u/6//0//d/+j/BADT//P/lf+j/yEADABxALn/+/8YAKv/2v8dACgADwAEABoAIAAiACQA8f/9/yAAAQD5/wMAEADo/yAA3P/h/6L/8/8eAOn/8P8WAB8AEAAJAA0AFgAdABEACAD2/yUAyf8XAP3/BwA8/xMAIAD//yQAAgAPABUAAwA5ABkA6//O/+b/FAAUAA0AJQDu/x8ALQAGABQAKwC9/wMAFAAAAN7/jv/7//L/CAC3//v/XgAWABAA9f/3/x0AFwAeACEA1f8zAAAAFAA3AHX/DwDZ/9f/OgDN/yQACgDn/wUABACj//j/HgAOAGoA7v8oABQAEgC8/wYAHwD3/+z/6P/h/+P/ef8hACEAHAAsAKj/AAAmAB4ACQDk//r/8v8uAAsAIwDb/xUA7/8aADQA4/8jAPz/DAAVAI8AMwAGAPf//f/N//3/CQDn/xsA8//5/xkAEwAXAPf/FwDz/+H/CwDl/wQADQCr/x4A//8MACEAGwAmACcA8v/Z/wsAEAAPAB0AFwAAAPf/YP/M/xkA//8oAPn/GQDI/+n//P/W/ygAAgD1/x8AigDP/ywABwAXAAEA5/8qAAAA6f8HAAwAGwAqAO7/CAA+AAQAIQAFAPv/IADs/wsALQA+AM//4f8UAPn/BgDw/xUA6/8QAPj/9//j/+v/sf/Y/+n/lP/w/9z/4/8CAOv/IgCR/6H/EwAnAHAA0f/3/xoAqP/m/x4AHwAIAAoA9f8bACQAHgAHAPz/HwAEAPr/AgAQAOn/LADc/9j/pv8YABoA5f/2/yYAJQAKAAgAFQAUADMACwD9/xT/AwDk/xEA9//z/wkACQAiAO7/HAACAAYAEwD7/zUAIADu/8r/3f8ZAB8ABAAYAA0ADgArAAsAGwAcALr/BAAHAP//z/+O//z/GwAUALf/AABjAAkACwDt/+P/GwAbACIAIADO/yIAEAAQAD0Aev/x/w4A9v89ANH/MQALAPD/BAADAML/7f8vAOr/agDw/yYAEAAUAMb/DgAPAPL//v/f/+D/2/92/xcAGwAtADAAr/8NABUAFgAEAN7/6v8jABkALQAoAN3/EgAbABMAMQAAACYA6v8QAAcAjQAVAAIA8f/7/8n/BQAOAOP/HgDx/+//CAAQACQAHAAbAOr/+v8HAOX/AQAJAKj/GwD8/wwAIQAcABUAHgDn/+L/CwADAAMAFwANAAAA9f/u/83/GwACAAoA9v8SAMf/8f/7/87/IQAAAOv/IgCKAMj/HAAKAP3/7v/r/zgA+//q/w0ABgAXAAoA6f8aADMAIQAlAP3/DAAeAPD/CgAnAEAA8v/0/xEA7f8CAP//EADw/xsA8P/n/93/5/+p/9f/+P+y/9n/2//i/wAA3P8BAJf/qP8PAE0AbQDP/+j/EwCp/9P/GAARAAYAAAAXACMALgAfAAkA+P8nAA8AAAALABQA6f9NANv/1P+p//D/HwDs//T/GgAWABUADAAEAA4AJgATAAMAx/8IAOn/FgD//wQABwAYAB0A//8hAAAADwASAPf/KgATAOf/x//e/xwAMgAJACMA/v8gACoABwARABkAvP8HAAsAAQDP/4n//P8RADsAtf8DAGEADgAKAPX/+/8cABYAGwAhAMv/GwAHAA0AMAB3/+7/FQDu/zwAz/8qABMAxf///wAA5f/o/xQA9/9qAOT/KgALABUAxf8CABoA8v8DAOv/5P/X/3n/JgAMADgAMACh/xEA/P8SAAkAzv/m/+j/HwAlABoA3P8WAOz/FwAwAAgAJwDm/z8ADgCIAB4A/P/y/wgAzv8DAAUA5P8WAP//7f8QABkAIAD+/wgAy/8WAAEA4f///w8AsP8bAOT/BgAfAAYAFQAUAPj/1f8WAB0A6f8MAPL/6f/z//b/oP/s/wQA9P/4/9X/zv/j/+7/zf8XAPL/0v8MAI0Azv8NAPz/7f8nAL7/KwD0/+r/IwAaAAwABwDt/wMABAAhACUA/f/6/xwAAAATAAgAPgAOAO//DQABAJ3/9f/W/wsA/f/z//f/BgDw/6//4v/8/3//CADd/+b/7v/0/xQAi/+m/x0AFgBxAN//AQAjAKr/2v81ACYACwD5/xcAIAAfABgAEADm/xAAAAAFAPf/CgAKAOj/iP/X/63//P8VAO7/2P8JABoAEgADAA4AFgAeAB4ABAAGAPX/7f8CAOv/DAALAB4AHQDy/ykA//8PABgA+f8jABUA6P+e/+P/7v87AC8AIwDW/wkALAAbAPv/NAC4/wsAJQAQAPD/kf/5/x0A7f+0/xUAYQD6/wYAAwD//y0AHAATACUA0f8XAOL/AAAPAHn/CwAUAPj/BADN/xcABAD+/wYAAgC2//X/GAD2/3AA+f8mABYAFQC9////GQD2/8X/3//i/9j/gP8AABoAJgAhAO//AQAAAB0AAgDJ//H/4P/8/xcAGQDk/w8A6//9/zAAAAAnAAQABAAQAHwA9f/0/+7//v/X//v/GwDT/x4A6f/s//3/MQAnAPX/DAD2/xwA+v/R//n/FwCu/yUA2v8QABsACwAjAB4A8//S/wsA9v/7/w8A8//r//b/9f+4//X/DwD7//n/2//E/+D////U/xoA8P/p/x0AjQDW/w4AAAD1/yIAsP8aABEA6f8iAA0AEQAJAP//EQAPACEAIwAGAPH/GQD5/xAADwA/ABQAAAAcAAAAFwAAANT/IgDz//b/+P8AAPj/rv/p//f/iP8PAOH/6//u/+3/GQCA/6j/GwARAGkA2v8oACMArP/V/3H/JQAIAPT/DgAaACMAHwAJAOz/IQDi/yYACAAUAP7/9P+//9//q//1/xoA9P/T/woAKQAXABkADQAWABkAJwAPAAsA9/8JAAUA//8XAA4AFQAZAO7/OAAAABcAEwD7/yIAEQDh/77/1f8EAEEALAAnANj/FgApACAABQAbAL3/CQAiAAsA//+R/+r/JQDx/7T/BgBcAP//EwAZAAAAMQAaABcAGwDT/xUA8v8LAAcAcf8HABgA/v8KAM7/DgAHAPb/CQACALL/+v8RAPj/bwAHACwAPgATALn/AAAeAB8AzP/V/97/1f+E//r/FwAcAA8Au/8BAAMAGwAAANf/+//m/wMAGAAqAOH/DgDv//v/LAACADMAEgADABAAgwD3//f/+f/+/8L/AgAoAOb//v/d/+v/BgAXACYA+P8HAA0ADgD3/9z/4f8RAK3/JADT/wAAIQAVACEAFQD9/9z/DgAJAAAAEQD2/+3/+P/0/5n/7f8ZAPv/+f/t/8r/5P/7/9T/GwDw/9v/HACNANv/FQAaAPn/HwDW/zAABADr/yQA//8DAA0A+//6/wkAGgAmAAQAAgAbAPb/EAAYADoAEADt/xgAHgAAAPT/5//9/w8A9f8AAPv/8v+q/+r/8P+Y/+r/3v/u//P/3v8XAJT/pf8QABMAcQDB/+3/EwCw/9z/OAAdAPH/5v8ZABoAFwAkAAIA8f8QAPv/CwAQABIAMQDs/8j/4P+p/wYACwD0/8P/HwANABUAGgALABUAHwAdAAkACQD4/+P/DQD1/wsADwAaAB4A9P95/xAADgAYAP7/JgAVANH/vv/Q/xkAOwAdABEA3P8QACQAIQACADkAv/8MACwA/P/s/4//+P8cAPb/tv8JAGQABgAMAAUA+v8sABcACQAYANj/HADt/wgACgBy//T/EgAAACMAyf8jAO7/+/8IAAAAw/8EABsA9/9xAAMAOAAcABEAuP8JAAsA+//F/9//3//W/4H/BQAXAA4AGgC1//n/CAAgAAIAyP/q/+3/AAAWABgA5v8TAOr/+P8qAP//IwAQAP7/CQCCAP3/+//z/wIA0f8FACgA5f8XAOH/8//Y/x0AGAD8/wcA/P8GAP//5f8HABAAqf8oAOP/7/8fAA8AGwAWAPH/0v8FAA0A+f8QAP3/8//y//b/fv/w/xEA+P8MAPT/xf/o/wsAxf8gAPX/5/8UAJAA2v8jAAMA+f8LANf/IwD2/+n/IQDu/0EADQD9/wwA+f8NACoAAgAVAB0A/f8VACkANgALAOr/CwACAAYA9f/u//X/FwD+//f/9P/2/6z/8P/0/4b/7f/j//D/+P/d/x4Afv+m/w8AFgBuALb/EQAYALL/4/8oACIA9P/3/xYAGgAbACEAAAC+/yUA/P///0YACgAxAPf/0//h/6n/BAAiAPT/6v8WACEAFAAIABAAFQAfABYACAAGAPb/3/8VAAIACAAIAAAAKwDn/zoA6P8TACMAIQAjABwA5P/J/+//FAAzABsADQDk/xYALgAfAAkAIADB/wwAHwAEAAEAkf/v/w8A9f+6/w4AXQAKABQAAgAFACgALwAiABoA0/8vAPL/CQAdAGr/9/8MAPX/OADQ/xcAAAD0/wUA/v+8/wQAGAD3/24A/P93/xoADgDB/wsALQD6/8j/1v/m//P/gv/8/yAAGgASAK3//v8NAB4AAADZ/+v/6P8CABgAGQDi/xYA6//Z/zIAFAAtAAAAAQDy/4kA8v8EAPv/BQDI/xf/GgDk/yAA4//u/wAADQAZAP//EQAcAMX/AQDi/wMABgCp/xcA7P8OACUAFgAbABsA7f/a/xAADQDt/yUA/f/r/x8A//+N/wcAHAD6//D/AQDB/+f//P/F/ykA/v/l/xkAjgDa/xEAAgD4/xAA4P85APL/6v80APT/CgAMAP3/BwAcAAIAJwD//+7/FQAIAA4ACwAyABYA5P8dAPT/CADr/xsA7/8QAP3/9f/t//L/r/8AAO3/lv/0/+f/6v/4/9T/KwCD/6P/LwAYAGoAuf8BABcAtP/h/xgAHADe//r/LAAYABQAIAACAOz/GAD7//7/CQAIACUA+P/S/+H/qf/8/xYAEQDr/xcAGQAPAAYA8v8WABgAFQACAAMA9f/Y/xcA8f8HAPX/DgBk//f/IwARABIAGwABAC8AGgDj/77/5f8LACEADwAaAOv/FwAuAGQABAAjAL//CQAXABAABACI/+L/IQD+/77/CQBeAC0AFgD9////IAA3AA4ARQDP/xYA6v8MACYAaf/y/wQA/P83ANH/LgAMAPH/AQD//7H/AgAeAPv/cADy/zEAFgD6/8v/CQAaAPn/4//P/+v/2P9///z/KwApACQAtf/+/wYAIQD9/9b/8P/w/xwACgAfAOL/BgDy/5b/MQAAABUACwAAAA4AiwD9/w0A8////8n/4P8fAOT/FADx/+3/AAALABgA/v8QAN//AADr/9//BQAAAKz/JwD0/wsAHAAWABwAGwDu/9T/DgAHAOj/Z/8DAO3/9/8QAKz/CQAHAA0A9/8RAMT/5f/z/9D/GAAHAPP/FQCMANX/FQAJAB4A/v/b/ycA8//2/xwA+f8EAA0A6/8PABYA8/8mANr/BQASAP3/DgAMADMABwDf/xIA/v8AAAUACwD7/wsA+v/3/+H/9v+v//n/7/+p/9j/4f/e//v/9/8MAIz/pP8wAAgAbADB/+f/FACv/9r/GgA3AAEA//8eABoAIQAlADEA8P8kAAAAAgAHAAgABQALANb/4v+n/wAALADr/+f/GQAZAAkAAwDl/xUAIwAVABEABwARANT/FQDz////EAAYACYA+P8eAAIADgAZAAEANQAfAOf/vP/c/w4AEwASABsA8/8nAC4AJQAMABcAxf8JABEABQD5/4v/7P8YAAIAvP8MAGEAIwAUAP7/9/8fADcAGAAfANH/DAAGAAcANQBr//v/6f/d/z0A0v8dAAUA5/8JAAAAov/9/yQACQBuAOn/LAAVABIAyv8EABAA+P/n/9D/4v/j/4X/AwAhACwALQDB/wUAJgAhAA8A0v/4//r/BABwACkA3v8tAO3/zf8yAAgANAD2/wkAEwCNAB4AAQDy/wUAzP/P/xgA5/8ZAPb/8//5/wYAFAD9/xcAGQAJAPT/4/8HAB0Arv8YAO//CwAbABEAGAAjAPH/1f8SAAQA/P8kABoA+v8BAEn/v/8bAAEAIQABABYAzP/j//P/1f8kAP//9v8bAIoA0v8ZAP///f/y/+L/KwDy//P/HQD+/xsAKgDz/w4AHgAKACIA//8JAB0ABgANABEALgDw/+f/DQD9/wAAFgA1AOv/EQDv//P/RP/u/7H/7v/0/77/CgDi/7T/+v/u/xAAkv+k/y0AHwBpANj/9v8ZAKr/0P8ZABkABAABACsAGgAeACIA5f/i/ykACAD+/wsADQD8/0IA1v/f/6X/FgAMAPr//f8nABUACgAKAKT/DgAYABYACwD+//v/5v8WAPL/8/8OABQAHwDz/xkAAwASABcAAQAxACQA6P+z/8v/GgATAAoAHQANABoAIgAjABsABwC//w0AEgAKAOz/k//l/yoABQC4/wMAYQAqAAkA+//J/xwAIgAdACMA2v8ZAAQADABFAHn/AAAKAPj/PgDO/yIADQD7/wcA///L//r/MwDl/2oA9f8dABQAEwDQ/wEAAQDv//n/4v/f/9r/fP/0//n/NgAzAKT/AwAWABgAEQDf/+v/IQD+/wgAGADj/xsAGQDU/zIAEQAfAAAADgANAIcA+f/8//D/+v/M/wYAFgDq/x8AGAD1/wgACwAeAAAAHwDt/xAAFADs/wQAAACq/xwA//8LABUAHAASABgA8//O/xAABAD+/xcABQD7////BgCy/xUACAAFAPf/DwDG/+f/8v/R/yEA9//t/xkAjADN/xQA/P8YANz/9P8lAPb/8P8kAP//EAAPAOP/DwAJAB8AJQD3//f/HwD1/w8AJgA8APz/6v8TAPL/AQD9/yAA7v8XAPj/6v8CAOj/sP/h/+n/r//p/+P/AAD//9//CQCT/6n/FABWAGQA1P/s/xMAo//z/xoAHgARAPr/KQAhACAAGgADAPH/JwAKAP3/AwALAPz/CwDW/9L/rP/r/x4A3//3/xcAKAAPAAMAyf8LAA0AEwAPAOz/8//v/xIA8P///wUAKAApAPf/IwABABMAFQD1/yYAJADd/6H/2/8VACQABwAlAP7/HQAwABIADQAnAML/DAAWAA8A9P+G//r/EQA0ALL/EQBYAA4ADADw/wAAHAAmABYAIQDR/yAA//8DAEMAdP/5/xoA8/9AAM7/EAAMANn/BAD8/+T/8f8ZAPD/bQDn/ysACwAdAMP/AAAZAPj/9f/g/+P/5f9+/xcAAQA0AC8Aq/8XAOv/GwATAM3/2//o//j/CwAkAOX/IwDo/9r/MAAPADYA9/8XABEAgwD7/+7/6P/b/8f/BQATAOn/IgAXAOz/DwAEAB8AFAANAOb/FQD//+j//f8UAKv/HQDl/w4AGgAQAAwAEwDv/+7/GAAjAPH/7f/r/+P//P/q/5n/BQAAAPX/+v/k/8r/5v/s/9P/GgAHANv/C/+NAM3/FwD5/+f/PADS/ycA9v/q/yUA5P8VAAQA8f/9//n/IgAoAPz/5v8dAPf/CwAlADMADADg/wkA+P/C//j/z/8AAPT/+P/t/woA8P+s/97/CQCa//v/z//e/wEA7f8dAIb/ov8KAAAAbgDZ////DQCs/+3/NAAXABYA9/8LAB4AIwAUAA0A//8mAPH/9f8SAAoABgDm/4z/2/+w/+r/CwDt/+r/BQD//+3/BAAUABUACgD7/w8ACAD0/9b/9v/o/wYADQALAPf/+f/9/+//DwAZAPv/KgAXAOD/h//q/+3/OwA6ACcA1P8QACkADwD4/xQAtf8HAAkAEAD0/3f/AAAKAOT/sf8XAFUA/v8LAAAA/f8mABoAEwAjAM7/JwDc/wEAEAB1////DAD4//T/y/8lAAAA+P8AAAIAq//v/w0A+/9qAO//BgAcABUAtv///xEA/f9s/9//4P/P/4L/AgAhACcAEADl/wQAAAAYAO7/xv/z/+X/AAAkAB4A5v8HAPP/BQAxAAQANAAcAAMAAQCEAPT//f/u/wIA0f8GACoA0P8HANb/7v/8/x0AHgD2/wQACAAWAPP/4v/+/wwAr/8dAOH/DQAlAAsAGgAQAPP/8f8SAAkA+//c//b/4f/7/+j/pP8EAA4A/v8DAOL/yv/k//T/2f8YAP//1v8jAJIA0/8VAAIA5/84AND/JAARAOj/MwAUABkAAQAJAAsABQAuACIA///b/xwA//8PABQAOQAFAOn/CQD8//b/AADY/xcA7f8AAPr/CQD4/7L/4f/4/6T//f/Z/+P/7f/X/yIAjP+p/xkAAwBzAOP/9f8jAK3/6f8u/xMADwDv/woAHQAaAB0ADQACABoACAAVAAsAEwAgAOv/z//r/6//5P8VAPL/4/8PACAA7P8RAA4AGQAYABMABgAIAPj/7f8VAPP/CgAOABYA8//0/zYA+/8KABQABQAkABYA7P+K/+b/3P86ABwAIADU/x4AJgASAPf/KQC2/wkAEQASAO7/ev/q/w8A7v+v/xYAXAAFAAoAGwD+/yoAGgAeACkA0v8WANj/AwAbAHP///8OAPT/AwDL/x8ACQDz//3/AAC4//z/EAD2/20A+/8AAC8AEAC4/wIAFQAaAGr/6f/f/9X/g//+/yEAHwAHAMH////1/xgA7P/M//b/5P/9/xoAGwDj/xQA/v8LADIAAAApACQAAAAKAIQA+P/2/+///v/E/wcAJQDi//z/2v/p//z/HgAfAPf/DwAMABMA/f/f/+P/CwCq/yAA3f8KAB4AEAAVABsA7//i/xIA///+/+r/+P/l//T/2/+S//r/EAD7//r/6f/J/+j//v/R/xEAAQDq/xsAjgDW/x8AHQD2/zIA2f8dAPj/6P8iAOr/CAAFAAQA///u/yMAIAD8/9X/HAD6/xYAHwAwAAkA4v8HABwA8v/8/+D/+f8FAPr////9//X/tP/T//v/qv8KAN//3v/w/8v/FAB8/6b/EAASAGsA1v8OABgArv/n/zcADAARAPP/EAAYAB0AHQAGAO7/GADs/wAAEwATABoA8v/H/+X/r//x/w0A9f8dABEABwDl/xYAEAAXACoAGAAAAAYA+v/T/wgAAQAJAAUADgD4/+n/Pf8IAA4AHwABACUAFADY/7r/7v/2/0IAHwAfANj/EAAfABgA/v8wALv/CwAkAAQA8P94//L/FQDt/7f/CwBZAAcADAAMAPr/KgAOAAsAGgDT/yQA4//+/xsAcP/2/wcA/f8lAMn/FQAJAPH/BAD+/6D//v8gAPz/bwD+/zMAHwAQALf/AAAoAPb/zv/n/9//2/+B/wYAHgArAAcAuP8EAPz/HADx/83/+f/u/wEAGgAtAOb/DwD3/wcAMQAIADEAEgD9/wEAhgD7/wAA9v///97/FQAeANX/DgDY/+T/5f8YAB4A+/8EACAAAAAEAOj/AAALAKv/HADc//H/GwALAB0AHgDs/9b/FwD6//7/5//8/+n/+v/l/2j/+P8JAAAACgD2/8j/8v8QAMX/EgD4/+//FgCQANv/EgD///n/JwDP/zAA+P/r/zAA/v8SAAUABQAQAOH/IQAnAP3/6f8cAPf/EAAVAC4AEADw/wEA9P/5//T/8v/3/xEAAQD2/wUA+f+y/93/+P+a//z/3P/V//n/0/8QAIb/o/8YABgAbADO//7/EgCx/9f/AwAXABEA9v8OABwAHQAcAAsA1P8PAPL/+/9YAA8AVQD1/83/5P+r/+n/BQDw/+H/FgAfAOH/DQAGABIAHAAWAAUABQD4/8T/EgAEAAsAAgAsACIA8v8yAOj/EAAkABwAKAAbAOj/zf/b//T/OgAbAPn/3f8RACcAEwAHACwAt/8NAA0AEQDc/3T/7v8eAPH/tf8RAFkANgAOAAkAAQAmACMAFAAgANT/GADj/wAAFwBu/+z/AQAFAD4Az/85AA0A+f8BAPr/uv/6/yEA+v9sAPz/Pf8oAA4Awv8HABoA+f/R//7/5//h/3j//P8nADYAEQC3/wUAEAAcAOn/1v/1/+v/AwAfABgA5f8pAO3/FAAzAAAAIwD2/wQADQCFAPf/FADv////z/8LABsA1P8HANz/6f8IABkAKAD7/wcADADg/wYA3f8DAAQArP8dAOP/CwAiABwAFAAHAO7/7P8SAAcA8/8dAP//6/8YAOv/c/8KABAA///7////wP/q//n/yP9lAAcA7/8RAJAA1v8aAAAA8/8MAOT/JAD6/+b/JQAIABwAAgD//w8A/v8QACEA/P/e/xwA/f8UAA4ALQAGAOT/EQDu//7/9/8DAO7/DgADAPP//P/2/7P/8//w/5b/8f/b/9n/9//k/wMAj/+k/zgADwBqAMn/6f8QALP/5P8MABwACQDw/wwAHQAYABkACwD3/xsA9P/6/wwAAAABAPP/0f/e/6//BQAWABAA4v8VAAgA3P8KAA8AFwASABEA9f8EAPv/1P8RAPf////y/xwATv/y//P/BgAQABYACAAxAB4A5/+5/+P/AgAhABkAEQDk/xQAKgAcAAQAGwC8/woAAwAQAPv/c//c/wYA8f+8/wwAVwAfAA4AAwAHACEAOgATAB4A0/8kAOX/BgAeAGj/8f/4/wIAQwDQ/wYACADy/wUA/v+8//7/IAD//3AA+P8qABwAEgDE/wgALgD6/97/1P/v/9j/fv8EACMAQgAgALv/BQAIAB8A5v/c//b/6f8XACEAMwDh/ycA7f/F/zIA+P8wAP7/AAD2/4gAAAAZAPX/+//M/w0AGADS/wkA6f/s/wYAFAAfAPr/BwAlAPr//P/f/wcACACr/xUA5f8LABkAFwAhACAA8v/v/xMAAADr/2r/EgDl/wAADQC7/wMADQAFAPn/DgDA/+3/9P/V/wkA+f/0/xAAjwDa/wYAAgANAAAA3v8vAPv/+P83AP//HAALAPn/CgAAAPz/JAD6/+f/HAD4/w4AMQAoAPT/6f8JAPf/+////xYA9v8aAAAA7f/4//P/tP/z//D/tf8FANb/zP/1/+b/KgCL/6X/IAAWAGkA5f/2/xIAtP/P////FQARAPX/LAAeABcAHwACAPL/DwD8//n/DAAKABEA+v/U/+H/pv8AAAkA8//p/xoAGADU/wYA9v8YAB0AFAABAPj/AgDM/xAAAAAAAP7/GAApAOv//P/6/xQAFgD//zIAJwDp/7X/4v/8/xMAFwAfAOz/LgAnAAoACwANAMH/DAASAAUA9v94/+j/GQD1/7f/FQBYADIACwD+//H/HgAgABsAQADQ/yMA9v8EAC8AZv/9//r/7/89AND/NAAGAO//BgD+/67/+P8qAPz/bwDy//7/GwALAMr/AQAPAPL/7v/a/+z/yv+B/wwAHABFAEIAuP8CABQAHgDi/9X/9//u/wcA9f8TAOT/LwDt/zUANAD3/yYABgAJABQAhwASAAwA7//5/9X/FAAVAOH/DwD0//b/AAASABkA9f8MAPb/GwD8/9b//v8NALD/LwDr/w8AJAAnAAwAEwDp/+7/FgD+/wAAJQAMAPL//f+k/7L/CQD0/wwA/f8KAMf/6//1/93/EwD///n/GACNANT/BwD9/xUA5f/m/yoA8v/z/x4AAQAeABkA+P8PAPT/FwAkAO7/6v8gAAYAFAAWADAA7P/d/wkA8P8BABUAHwDq/wwA9//v//H/8v+5/9L/8P+u/+v/3v85AAIA4v8JAID/pf86ACcAZgDl/+X/DACt/9z/DAAVAA0A/P8JAB4AKQAaABAA5v8TAP3/8/8FAAsAAAAtAMv/0/+m/w0AGQDu/+v/HQAAAOX/AAACABwAHwARAAwA+f/4/9P/CwD2//f/CQAbAPT/7v/r/wMAEgATAPb/LQAlAOP/mf/T/wgAFgAWABgA//8lACIACgATAB0AuP8MAAsABADV/3z/9P8SAP7/sv8SAF8AIQAGAPr/0v8UAAcALwAiANH/HAAJAAIANgB3//r//P/r/zkA0P8MAA0A9v8UAAAAx//z/1oA5f9sAO7/9f8ZABUAy/8AABYA7P/p/9//6v/U/4H///+8/yUAPgCz/wUAAAAgALn/0P/r/xAA9f8UACsA5f8jAAYAFQAqAAAAJgD+/xAADACIAP//BwDr//X/0P8FABYA2v8TAAgA8f8AABIAJADy/xUAFAASAAAA5f8BAAUAsP8bAPH/EgD2/xkABQAPAPn/5P8NAAkAAADo/wsA7f/6/wUAxf8FAPT/+v/0/wgAzf/q/+n/2f8XAPL/+P8XAI8A1/8OAP//7v/K/+7/LADx//H/KwAEACAABQDx/w4A6P8kACUA///l/xoA8P8YAB0ANgD0/+H/CwDn//v/CgAXANr/CwD+/+3/3f/u/6//1v/w/6H/8v/Z/w4A+v/R/xgAmv+n/yoAZwBlAO//2P8TAK//GQAKABQAGQDu/wsAHgA6ABkAEwDu/xoA+P/v/wkADwDy/xsAyP/I/6j/7P8IAN7/9P8IAAgACgAAAOD/FQATABoABgDq//P/4f8MAPD//f8IABMA+f/i//n/8f8UABkA4P8kACkA5P+G/9n/AgAkAA4AHAD0/y8AMAADAAUAHwC5/w8ABAAHANP/aP///xgAJQCx/xMAVQArAAoA//8CABcAEwAfAB8A0P8RAPj/AAAxAHv/AgAFAOP/OgDO/w8ABwDY/+3//f/l/+H/IgDx/28A6v/r/xEAGgDD/wAAEgDy////5v/o//L/gf8IAJj/OAA+ALb/HQD7/yYAtf/L/+P/5f/6/yAAEwDp/yMA4/8OACsABQAjAOz/FQAOAIUA/f/9/+j/t//C//z/FADV/xYAEADt/xIADAASAPT/DgD0/xYA8//m//L/CwCt/wwA9P8RAB8AGAAPAAoA9v/r/xgAygDw/43/8P/X//T/aP/X//L/AQD5//T/6/+9/97/+P/P/xUA9v/h/3D/gwDQ/xsA/v/p/zgAzf8mAO7/6f8dAAUAHQACAPX/BADw/yMAHwD9/9r/HQD3/xEAGQAsABAA7P8LAPj/3v/z/97/BAD+//T/7v8GAO3/t//F/wgARP/0/9H/1f8AAOn/FQCa/6L/EwAbAGoAEAAAAGUApf/u//X/FQAQAO//CgAWAB8AEwAMAPH/KgD1/wAACgATAAUA6/+T/9j/rP/v/wwA7//u/wMACwAWAAUAEwAMAAMA9P8AAAgA8P/X//b/8v8DAAwAJgCM/+v/fv/4/w4AFwAAACQAHADe/9f/4v8CAD8ANAAfANL/EQAYAAoAAAA8AMT/CAAIAAkA0f96/+f/FgDs/7D/FQBYAPb//v8HAPv/JgAUAA8AJgC4/xQA4P/6/xgAYv/3/xUA8f8XAMz/EQADAO3/CgD9/9L/+v8lAPT/bgDx/4r/GwAQALj/+f8aAPb/lP/k/+D/1P+N//v/GgAyAAsA9f8FAP//HwAEALr/8//o/wAAHwAVAOX/CwDq/wMAKAAJAB0ACQABAAgAegDz/wwA8P/6/8T/BwAcANz/AwDZ/+v//v8jABoA8/8bAPj/EwD+//j///8MAKf/GwDt/w0AHQAYABcAHgDz//T/EAAqAPX/h//7/+X//f99/9H///8JAP//AgDk/8X/3v/9/9v/GQAAAN7/JP+JANL/EAAJAO//OwDY/x4ADADq/zgA8/8fAAcACAACAAcAKQAjAAMA0f8bAPz/CQAZACkADQDx/w8A/P/6/wAA2f8eAO3/+P/0/wcA9f+3/9L//f+T//v/2f/j//H/6v8PAIL/qf8MAB4AaAD7/wwARACo/+v/l/4hAA0A+P8TABUAHwAaABAA/v8xAAAAGAAQABkABQDr/8//5v+w//H/EQD0//D/CwATABMACQATAA8AJwAQAAUACQD1/+X/HQD+/wsADAAeAJP/6P/5/wAAEAAYAAgAKQARAOj/qv/j/wwARQATACEA2P8UABwAFAADAB0Avv8MAAsABgDn/2z/3P8ZAPD/qv8YAE0A9v8JABsA+/8pABkAEgAnAMP/JADh////EgBo//f/FQD0/xYAyf8WAA8A8/8GAP3/vv/8/yQA9/9wAP3/mf8yABcAt/8FABkAFgCH/97/3f/T/4z/8f8aADUABADG/wkAAwAdAAQAyf/6/+v/DAAWADEA5f8RAPT/CwArAA8ALwAiAAcADQCCAPf/BAD5//f/vv8MACcA5f/0/9r/6v/2/xwAFwD7/xQAEgARAPP/8v/m/wwAp/8SAPD/CQAfABgAFwAWAPD/8v8WAAYA//+I//r/4f///3r/yv/3/xUA/P/2/+n/x//k//z/0f8ZAAAA5f8kAIMA1v8cABMA8v85AOH/IgD2/+n/KgDw/xMACAD4/+7/+v8fACUAAgDU/x4A9P8RADAAIwAPAOD/AgAXAPn/6v/x/wEAAwD5//n/BAD3/7b/zP/7/6z/6f/f/9X/+P/R/wwAhP+i/x4AGwBvAPj/CwD4/6v/5P/r/x0ACgD0/wsAEQAYABoADQD4/wYA9P8AABAAGQAMAO7/yP/p/63/+//9/+7/+v8SABEAEwAWAAwAEgANAAgA+f8DAPf/yf8GAP7/BwAFACMAlP/s/5v++/8JABgADAApABcA2v/G/93/+f9CABcAGwDX/wsAIAAUAAAAKAC+/woAHwD//93/f//i/wgA7f+v/w8AaADt/wUABwD+/ygAFgASAB4Awv8iAOT//v8TAGv/7v8UAPn/OgDI/yIAFwD4/wMA+//A////IwD5/3AABgD9/w8AEAC6/wEAFAD1/8H/8f/Z/9X/jP/5/xsAJgD6/7z/CwAEAB8AAAC3//L/7/8AAB0ACADj/xUA8v8EACsABgAgABUAAwAHAIMA/P8FAPL/9//M/wYAIQDW/wAA2//t/9f/HwASAPX/GQD+/wYA/v/v//v/DgCl/xQA7f/6/xoAFwATABQA8v/l/xQA/f/+/4n//f/o//7/ef++//D/EwD//w0A8v/D/+v/CADJ/xAA+P/r/xMAhQDZ/wwABAD9/zYA5v8XAPb/6v8vAPv/EAAFAPv/CQDq/x0AJAD+/+L/HwD5/xEAFwAhAAgA7f/6//r/+//y//f/+f8OAPn/8P8EAPn/tf/P//P/pv/s/+H/3P/7/+D/GwCH/5z/GQAhAGsA6//5/+n/rP/m/43/DAABAPP/CwAVABAAGwAIAO3/GQD6/wEAZAAXAAQA9f/R/+f/qP/w/woA8f/6/xIAFAAQAAkABAAOAA8ADAAAAAQA/f/M/xUACQAFAP7/WwDy//D/9v/m/xIAIwAfACsAHgDo/7n/5f8RAD4AEgAFAN7/EwAiAA8ABwAXALz/CwAJAAIA4P93/9//EADv/6//DQBZAA0ACgAFAAIAIgAaAAwAHgDE/xYA6P/9/x8AY//q/xAAAABKAMz/JQAJAPP/BAD8/8f/AAAlAPv/cwD0/6n+CwAMAMj/CQAXAPr/y//l/+D/3f+E/wQAKQAwAAIAx/8GAAQAIAD9/7v/8P/x/wEAGQB/AOL/HQDo////LgAHACIA9P8FAA0AhQD9/xEA8//5/8H/BAAaANT/DADh//H//P8XABUA/P8PAAgA3P8CAN7/+v8GAKP/EQD2/wgAHgAvABMADQDt//v/FAD8//T/9/8GAOf/FQB5/7v/BAAeAAEA+v8FAML/4//+/8j//v/5/+j/EwB+ANj/FQACAPn/FADo/ygA+P/u/ywAAAARAAMA+f8IAA4AEgAjAAAAy/8ZAAAAEgAWABsABQDl/wQA8////+7/AADz/w8AAADy/wAA+P+z/+D/7f+h/9P/4f/d//P/1/8bAIX/mv8WACIAaQDm//j/8v+v/9n/iP8WAAMA9f8OABAAFwAaAAkA8P8gAPf/AAAKABIADQD1/8v/5/+p//n/FAAPAO//EQAWAAwACQAFABMAFAARAPb/AAD0/8T/EwAAAAcA5f8wAIj+7/+M/woAGwAZAAsAMAAfAOX/wP/g/wMAMQAUABcA4v8kACgAGgAJABUAvf8JAAUAAADj/3X/2v8PAPL/sv8JAF8ABQAJAAUAAwAdAC8AFgAhAMT/FgDn/wIAHwBh/+//CgATAEMAzP8YAAkA8/8IAPz/zf/8/ygA//9vAPf//P8TACIA0v8OACAA+P/R/+b/4//S/4b//f8YADQAKADK/wgADgAhAPr/w//1//L/FwASAAMA3/8bAO7/2/8rAPj/GgD3/w4AEwCKAAcAHgD1//b/xP/5/xgA1f8QAPL/8P8BABcAGwD4/xYA7P8IAAIAyf8AAAgAqf8YAPX/DAATAGMAEAAWAPb/+f8UAPf/9/8I/wgA6P/3/9j/1P/6/w0A/f/2/wcAw//q//b/zv8dAAoA7v8fAIQA1/8UAAAABgDs//D/FgD3/+v/DgD+/xMACgD8/wQA8f8JACIA/f/f/x0A/P8NABYAGgD6/+T/CQD2//r/9/8dAPD/DgD1/+r/AADv/7T/yf/s/7T/0//W/9n/+f/k/xQAjf+h/y0AFABrAAEA8v/v/6z/3/+F/xQABQD2/xwAFAApABQABAD2/xcA9//+/w8AEAD6//X/y//V/6H/9/8pAOv/7/8NAPj/CQAEAAUACwAJAA8A//8CAAEAxv8UAPT/BgD3/x8A8v/p/47/9/8TABsAAAAyABUA5f+u/+b/BAAgABYAHQDh/z0AJwAMAAkAMADD/woABwAJAN3/dP/h/xoA9f+0/xgAYgAIAAgA///8/xkAEgAWACQAt/8OAPP/+v8pAGT/9f/r//T/QADO/ycABQDw/wcA+//I//v/LwD4/28A6/+M/wsACwDN/wEACAD3/83/3P/n/9b/i//+/xIALwBEAMr/CQAPACMA8/+7//T/7/8EAAQAKgDk/xUA4v8EAC4ABQAoAOn/AQAMAIMADAAZAPD/+f/G//z/FgDf/xEA9P/x/wYAFgALAPL/GgAQAA8A///H//v/FQCs/xYA/v8SAAwAHgAZABMA6f/s/xcA/f8AAO//GADp/wUAKP/J/wsABwAcAPz/CADI/+n/+//a/yYAAADy/xYAiQDW/w4AAAAAANT/7f8qAPL/8f8bAAgAFwAXAO//DwD1/xMAHwAHAOT/HgAAAAsAMAAgAO7/4f8PAP3/AAAKABkA8/8WAAwA8P/l/+7/t//L/+7/pP/w/9P/LAAFAOb///+K/6L/FQAsAGQA///u/wAAp//u/4j/JAAPAAUAFQAXAEMAHAAPAOv/FQACAPz/CQASAP3/MQDM/93/nv8fAA8A9P8AABwAHQAPAAoA+f8NAB4AEgACAPr/9v/Y/xQA7f/q/wIAJACL//T/hP8AAA8AFwD0/zEAIgDf/4z/2f8UACIAEQApAAIAFwAlAA4ADQAjAMH/DgAKABAAyf9f/+b/GwAAALP/HABcAA0AAgABAOH/FAAWACUASQC7/xQAAwADADQAcP/6/woA7f9EAMv/FwARAPL/EgD//9//9v9zAOH/bAD0/4z/DAAVAMv/BAAcAPb/3//m/+r/3v+L//v/9/43ADgAuf8KAAMAIAD1/8X/7f8ZAAcAHwAcAOb/LgABAAoAJwADAB4A8P8LAAoAgQAGAAwA8f/9/7n/BwAXAOL/DwD3/+//CgAPAA8A+v8aAPv/DwD5/9X///8HAKn/HgD6/xMAIAAeABIACwDz/+P/HwD+/wQAjf8PAOf/9P/S/8L/AAAAAAIA+P8GAMT/6P/z/9T/KQAGAPv/FwCKANj/GQD9/wAAzf/x/x0A8v/t/yQABAAhAAkA7f8GAO//JQAhAAAA2f8gAPb/EQAkADAA9f/p/xYA8f8BAP//HQDm/xcABgDr//b/7P+1/9H/7f+I/+v/zv/d/wIA4f8cAIz/ov8XAHEAYwAEAOj/AQCq/xgAjf8PABMA/v8WABcAM/8bABEA9/8cAAYA+P8MAAwA9f8LAM//1v+h//P/FQDt//v/CgAOAB0ABQDs/wkADAASAAgA9//5/+D/EwDw//z/AAAnAIn/8P+T//b/DQAWAPL/JQAmAOD/m//i/xIAOAASACcA6v8pACYABgAMADQAxP8MAAIACQDU/37/5P8ZAB0Asf8VAFEADAABAPr/AwAUABcAMAAvALj/HgDv//3/MABs//j/GADi/0oAzP8YAA0A1f/+/wAA6//2/y4A+f9rAOT/mP8HABoAxv/9/xwA9//p/97/4//z/4z/BQBj/zgAOQDC/yAA+f8lAPn/u//l/+3/AAAgACgA6f8SAOv/FQApAAUAIwDv/xIADgB4APv/AgDx/+r/s/8NABkA4/8XAAEA7v8LAAsABgDy/xUA8f8PAMf/oP+Y/y4AWf8gABwA//9IACkADQAVAPX/wv/D/x8AVQAHACAARAA4APf/jP86AE0AFQAXALH/cgBc//7/oP86ABwA2P9dAN4AV//J/wAA8P8XALb/eP/M/+P/TAAOAPX/8P+//5AA8f8mACIAIAD6/zQAVAAMAEIAmQAWAP3/CAAxAMz/2v8XAAgAGwDo/ycABQAwAHb/DQARAD4A8v8JAO3/4v/5/xUAmf9g/yIAIwDv/ywADQBIAI3/DwAOACgABAAfABsA3v8iAEQANQDt/ywADgAAAB4AKQDR//z/s//B/17/BgAhACoAAAAbABIAQgBF/wAA7f8OAB4A8f8MABMA2v8DAPv/LQAKACgAEwApAAoAQgA7ADMAWgA8AEIApP/8/zUAJwBHABMANwDl//X/DQAZADgALf9r/xcALQAMAPP/uP+2/yIAFgBn/+3/oQAJADEA///7/x0A/v9TAAkAiP8fADsARgADACwA6f8nAOf/QwCK/0AA+v/Q/+b/AADO/zoAPQAPAKEAAQASAAEADACI/+r/GAAXALv/4v9S/+n/ZP/8/z4Arf5DAC3//v8OACoAGQD1/w8A/f8wAEAABwDE/xYA7v8iAE0A9v8qAOL/CABJALkACwAGABQA+/+B/wEA/v9M//T/8P8/APn/HwAMAPP/+//I/w8At/+g/6P/IgBr/wMAEQD5/y4ADAAhAAAAHADV/6z/EQBYAAYAJQAAADUAAADK/x4ANQARACMA6v9yADX/AgCm/z4AGwC+/0MA7AB4/9T/DgAMAP3/tP8gAP7/4/85APz/HgAOAKn/MgDr/xgANgAQANT/PADq/yQAIQCVABcA3f8aAC4A5f/j//b/AAD0//3/FwABAP//ff8FAOX/RwDd/x8A+f/v/+f/NwCj/3v/LgBDAOT/NwAFAEcAmf/m/6v/FwD5/ycAEwDN/zIAUAAIANr/KgAEABYAGgAHANr/AwCZ/7b/Z//6/xkAKwAUAE0AKwBEAAQAFgD0/x0AEwDo/xMADQDT/+H//v8kAAIAAwAXABoA/v8+ADkAMQBJADoAIgC8/wkA3/88AFUA7/8IAPT/EQAAABsAPQDr/17/EwAWAPr/3/+w/+L/HAAPAGn/1v+nAAQALgAmANv/KAD6/0sAHgCl/xEAJgA7AA8AKAD//xoA1f85AIj/QgAUAMj/zv/8/8v/PAA8AC4AqwD9/w4AEQAsAIj/GwAjAC8AwP/n/2//6v9e//L/JQA+/0UArf8GABwAJAAaANH/AQAJAA4ALAD//87/MQDs/woATAACABcA5f8nACgAygAJAAAAGQANALb/CAAGADf/+//0/y4ACAAoAAAA6f8BAMv/7//j/6//vf8kAHf/IgADAO3/MgAeAD8AIwA0AOT/xP8jAEwAFAAoAPT/GwABAJr/PgBGAB0A+v/s/3YALP///6z/NQAVALf/SAD8AGr//f8HAAcABQDA/wkA6f/d/zwA9f8AAPn/4P9OAPv/+f8zAAQAx/89AHz/GQAWAJQAEgD0/xcABQDh//z/HwAaAAwA6f82AO7/DQB9/+v/5/9NAOD/HwAGAO7/2/9AAJz/dv8eADYA5/8/AAcATwCH/8r/FQALAOf/LgAOAMv/JABGAAQA7P8hAAIAIQAnACgA3/8aAKH/xP9h/+//+v84APr/SAAvAD4AEAAaAPv/HgAWANL/CwArAOH/9P/1/ysA/P8KAA8ALwCu/ycANwApADcARgApAML/DQDv/zQAYgD//woAAQAQABYADABFAMr/X/8RABkA7f/1/8L/+v8iACAAc//g/6YA/v8vAAgA8f8sAPT/QQAdAJn/FQAvADQAHAAsAOD/HADW/zYAgv89AAkAxP/V//L/0v9CAD4AGQCvAPL/FQASABkAnv9RABsAAADb/+X/YP/f/2D/AQArADb/OACt/wgAKQA0AA4Azf8MABcA/v8pAAAA1f8qAPX/7v9KAAwAIgDP/ykAKQDOAAkACgAeAAkAq/8RAPb/Vf8GAOX/QwDa/yMA7//+/wAAwf8AAMX/uv/C/ycAfv8KAAsA2v8sAA4AKAAdACIA4P+w/yoAUAAJACoA6/8ZAPX/s/8vADsALgANAMz/dwBE/wcArf8zABEAwv88APIAaf/c/xoABQATAMr/HgDo/+v/MgDf/xIADQCr/1oA0v8MACoACADL/z4AHQAiAC4AjAAYAPj/DQAxAOX/+P8GAAAAAAAEABsA9P8EAHL//P/d/1MA2P8kAAUA8v/m/y4Amf9w/xoAPADg/z8ABQA+AIz/4v8RAAkA9f8pABQAfP8fAEcAAQDi/x0ABgAFABoAJADv/woAsf/I/2j/8v8AAAwA9v8/AB0APwAJAAsA9/81ABEA6////xQA1v/g//P/GAD9/wEAFwAcAAkALQA9ACYANgA3AC8Asf8QAOX/LwBaAAMA5/8YACIA7/8MAE0A/P9m/xQAEgD0/8v/wv/s/x8ALQB0/9X/sgDl/yMA+P/0/zIA7f84ABkAp/8AAC8ANQAhADMA2/8UANz/QAB4/zgAEACu/9n/9P/W/zYAOwBHAKwADgC1/woAKgCw/z0AGgAVAOH/4f9z/9r/ZP8HABgAxv47AKn/DgAnADAAGgDT//P/EQALACwACQDY/yoA6f8AAEkA9v8BANT/LQAZAMkACgAJAB4AAgCu/wAABgAs//r/8P80AAEADwACAN7/FwDW//D/uf+w/6f/TAB7/xkAAADk/y8AFAAmAAMAIgDn/7P/FQBlABAAOwDj/ywAAgCX/ycASAAXAA4Azf95AEX/+v+v/y0AHwC8/0IA6QBp/+3/FAD5/xEA0/8kAOn/+f8aAPX/BAAIALX/XAD2/wwANwAZAN7/OwDm/w0AHwCJAA0A+f8QABoA4P8IAAcAJwACAP3/DwDu/wEAb//v/9//TQDW/xgACQDr/9P/KQCX/2z/FwAzAOD/OwACAEcAeP/P/wwAGADo/ygAFgCk/ycAPwD4/9b/HwAPAC8ANgA5APP/EACv/8H/bv/b/+r/NQAHAEgAJAAyAA8ADAD1/zAAFgD3/xIABwDb/+n/CgBBAAAAFwC9/zEACAA/AC0AJgBJADUAEgC7/xQA4/84AFcABQAJAPD/DgCs/xcAUgCC/1//DQAGAPv/1//D////MgA8AH7/6/+xAO7/IgDw/9b/KgD1/ysAHACt/woATwA4ACEAOgDs/wkA2/9GAHL/OQAHAKb/3f/x/9b/HAA2ADcAqAAPABAABQAGAKn/PgAlABIA7v/H/2n/0/9b/+f/KQB5/zcAwf8NACUAKAD//8n//f8cABwAIwD9/9T/JQD0//T/RADf/ykAwP86ACYAxQATABAAJAAHAKv/CQAMAD7/8P/v/1IA8P8hAOb/9v8IAM7//P/x/6L/w/9OAGD/FwD+/+7/JAAQABMAEwAiAAYAkv8mAGIAof8EAKz/KgACAIz/MgAcABYAIgDh/3kAQv/9/6n/JgApAMf/RQDuAGz/xP8RABcACgDH/zUA9//p/zYA8v8HAB4AvP9KAMP/9/8zAB8A4v9HAAMAAAAaAI8A+v8SABcAGQD5//b/5P8OAP//DgASAOj/BgBs//j/4P9KAL//HgD1//L/sP8fAJX/av8pAEIA5v9BAPL/RgCH/9P/CgAfAN7/LQAcANP/JgBHAAAA1/8fAAQADAAlABYA3v/8/6T/2/9u//H/AQAHAAAAPQAbAC8AAAAnAOn/IAANAOb/BQASANv/3v8HAOD/AwD8/xIAGQAQADgAMgAIAEsALgAnALn/GQDI/zwAUQACAPz/KQAXAAkADQBRAOH/Yf8SABcA/P/p/7X///8xACgAef/m/58AAwAlAAAAyf8YANz/QwAcAJb/EgBNAD8AEAA3AO//ff/T/0QAff86ACUAm//Q//b/1/8uACsASgChAAAAEwAXAC0Amv83ACMAGQDk/9X/Xv/e/1L/AgApANP/PwDL/xkAMQAwABAA0P/2//7/FAAYAAcA1/8fAOv//P9CAAcAGwCi/yMAIwC/AAgACgAbABQAo/8aAAMAMP/5/+n/TQABACEA9P8FAAAA2v/3/+r/nP/A/0oAaP8eABQAAQA5AB8AEgApAEAA1/9o/yYAYgD6/ycAtP8dAJH/k/8qAF0AJQAjAAsAdQBN/+b/pf82ABAAuf9CAOUAav/E/wMAFgAMAM7/GQDd//f/LAAXACQADwCw/0MAz//r/yYABwD7/0MA9v8aABwAlgAKAAwAHwALAL//AgD8//D/+f/x/yAA8v/0/2z/9//l/0oA3P8jAPD/6//A/yoAmv92/xgAPADn/z8AGABAAIv/3P8EAP7///8bABwA2P9JACkABADG/ygACQAXACMAIwDf/xMAqf+1/2f/3f8jABQAAQBeADMAPgD1/ywA+f8qABsAGwD5/xEA+P/p//H/1/8XACcAFgA8AAIAPQAlACcAKQAiACcAqv8XAOf/OQBSANn/AgAwABsACAAeAGsA1P9k/xIAHgAVAMn/uv/x/y8ALAB+/9f/mQADADIA/f+h/yEA/P9JAAwAiP8XAFEAPwBDAC4A9f8KAMH/SACR/xcAJwCu/8z/AQC0/6gALAA+AKYAr/8aACEAJADG/z0AIwACAOT/tf9s/9j/ZP/2/zUAwv85AMT/EwAKAC4ABwDG/7X/IAALACMA/P/G/xMA5//+/0IAAAAIAMn/LwAuAL8AAwDq/xwAFACg/wMA8f9D/8//2f9mAOn/NwDi/w8A8v/a/wgAlP+o/8H/hABz/+//FgAIAC8AMwAaACEA0v/F/93/MgBPAAQA4//J/0UA+/+l/3YAFwArAAkANgByAFj/CgCk/zsAJgDG/zkA2QBC/5D/EQAMACYA/P9UANH/IQA9ACAALwAlAOf/agD3/wb/JgAbANj/RQAbABkARACVAPz/CQAOABIAtf8HAPb/6f8rAOv/FQDk//7/T////+j/QQDZ/yIABwD6/6v/AQCk/1r/LgAxAOX/MwAPAEcAh//E/wEAcP/p/0QAGADq/0wANAABAAAARgAQABMAPgAvAAAAMQC6/+L/af/f/wQAGwD+/zkANABKABUALADt/xoAJwABANL/OAD5/+r/BgDa//z/HwAQACQABQAqABIAEQAwADAA/v+h/w0A4v8xAF0A7/8vAFEARwABADIAagDi/mr/EwAmAP3/tf+o//P/MgBMAIP/6P+eAAgAMQABANv/IAD7/08ABwB7/wQAXgA/AD4ANgDk/wMAyf9HAIn/+v8YAML/FQD5/z7/jf9FAIoAoQDx/xcAHAAkAN//PwBgAN3/5v/B/3T/1/9e/+f/VAAS/zUAsf/7/2kALwABAOP/qf/U/zAALQAhAMr/IAA5APX/QQACAGT/zv8pAB8AvAANAPT/MgAJAH//JAD9/2X/NQD+/zkAEAAUAPr/EADP/wQACwD5/7P/uv8EAGn/BQARAAAAMQAlACkAAAAsAPD/xf8LAFAAz/8cABQAMADL/w0AHwAqABUAGgCm/4oAPP8QAKT/OAAaALL/TQD5AI//8f8WAAAAHgDX/zEA7//4/zkACQAbACcAt/8ZANT/JgArABoAAABOAL3/CAAgAH0AFgDp/wcAOwDv/+H/8P8FAAwAAgAUAAwACwCB/wEA0/8/ANj/GwAKABgA9v87AE3/ev8SAB4A5f8+APn/OgCO/+n//f8cAPb/IQAaANj/LwBOABYA6P86APX/DwAfACcA6v/3/6//tP9r/wMAMAAIAAsABAAgADIA+P8pAO7/KgAdAO//GQAaAOP//v8XADIAFAAcABEADAAFABsAMAA2AC0AUAAmAK7/DQDx/z4AXQAMAAcA3/8CAOT/IgASACgAb/8YAB0AEQDI/7f/zP8jAAQAff///7gAEAAbAAoA2f9LAAEARQAEAKL/GQAyAEEAAAA0AOz/LwDU/zkAgv8gAAwA2v/z//j/kP8eADMAIgDDAPv/IAATABwAkP9L/yoAKQDF/+r/c//h/23/DgAaAIT/PQCS/xMACAAbACgA4/8CAOL/EQApABgA0f81APb/5f9TAAYAHwD+/xAAMQDeAAgA8v8aABAAyP8SAP//e//9/+j/KQDP/zEACADp//7/0/8EANT/xf+v/xoAcv8SABkA+P84ACcADAAIACIA0v+j/wcAUgD3/x0ACQAuANv/OQAPADAAHwAmAMH/hABm/wQAqv84ABIAuP9GAPQAg////xgA7/8XANb/IQAAAOr/SQASABQAKwCr/w8Ax/8ZACcADgDM/0EAKAAXABoAgAAYANH/8/8vAAIA5/8NABMA/v8HAB8AAAAoAHv/AgDs/0UAxP8lAAUA6f///zsAlP9z/yEALwDm/04A6f9AAH7/3P+h/w4A9f8VABgAzf8jAEoAHQDi/yoA/v8eABwAHADl/wEAp//O/2n/9P8aAPr//f9hACoAQADx/x4A7P8uABEA+f8RABgA4//2//b/JQAAACAAFQAeAAAAIwAvADkAVgBNACEAxv8WAO//QABcAP//AADr/x4A9/8eAC0AGQBs/w0AFgD+/8f/xv/f/x8ABgB//+j/vAAGAC0A4f/U/0gACwA9AAcAn/8kACUAQQD5/zcA6/8mANr/MQCA/0YA///a/wkA+/+3/zEAOAAeALsAEAATABAAGwCP//L/GQAtAOH/1/9c/+b/ef8RABMAU/87AK//FwAUABoAIgDe/wAA9f8oACgABgDK/zUA3v8BAEwACQBCAP//GQAvANwACQD2/x4ABwC//w4A+f9b/+z/9f8/AOz/KwD8//v/AwDL/wwA1P/D/7j/IgCC/w0AFgDy/ywAHwASABEAJADc/6r/CABfAO//FgDm/zEA2P8pABAAKQAhAN3/w/+CAEH/EQCp/z0ADACq/0gAAQGC//P/FQAPABoAvf81APD/6v8sABAAFQAlAMj/MwDg/y4AKgAdALn/SgC3/wwACQB3ABgA7v8WABUA8f/6/xQAAAAAAAYALAD5/xMAhv/+/+r/TwDU/zcA9//2/+f/RQCY/3P/CgAvAOD/SADh/zsAlv/d//b/DQDx/xoAEQC6/ygATAAFAOf/NwD7/xkAJwAjAOP/AAC//87/bP/v/wUAGwAEADAADAA9ABkAHwD4/zQAEwDo/x0AIQDZ//z/6v8oAAAAFQASAB0AqP8TACsALQAdAEcALwDD/xsA8v86AFsABwDb//P/BwANABAANQAhAGP/EwAYAPv/6v+4/+L/JAAPAID/8v/HAPr/KwATAOb/SwD+/1AABgCv/y4AHwArAAAALwD1/yAA8f80AGv/DgDp/83/+v/6/8r/LAA7ACsAvQANAAQAFgAlAKL/PQA6AD0A+f/Q/3j/6f9h/x8ALwC7/jkAiP8RADIAKgAdAMb/AQD4/xsAJQARAM//LwD5/xMASwDr/y4A7f8yADAA4AAGAAAACQASAL3/AgAGAEL/DAD7/zkA3/8qAOz/6P8WAND////b/73/uf88AH//GAAFAOz/LwAbADMALwAkAN3/aP8XAFMA3v8cAPv/JgDl/xsAPgBGACMAFADH/4EAcf8LAK3/LwAgALb/QAABAWP/5f8dAAEACgDc/yIA9v/v/zgA/f8QACAAyP8vAMT/BwApABsAx/9HAAQADwAvAHcAGwDi/wAAIQD0/wEACwAZAAgA7v8jAP7/DgCF//v/4f9NANT/MQABAOv/2/80AH//ef8SADAA4f9QAPj/OgCB/8X//v8NAPX/JgAUANj/KgBJAAYA3v8wABIAJAANAC0A2P8NALT/zv9h/+b/EAA1AAYAQQA/ADsAAwAMAPP/NgAcAOP/+f8sAN3/BQDe/yoAAwALAAQAKgDr/ywALwAoADEARQAgALz/GADI/0IAUwAAANf/AAD//6H/FQA2ACYAZv8MAA8A9P/L/7//8/8rABMAf//v/8QAAAAnAPn/+f9GAAAANQAWAL7/HgA2ADgADQA2AOr/DwDT/0MAav8xAAIAyP/t//j/vP82AEUAKgDEAAcAu/8ZAAYArP9VACoABADw/+H/XP/u/2P/CAA6AIz/OACc/w4AFQApABoA5P/u/wEAHwAuAP//0/8qANv/DQBKAP3/KADI/ysAKwDcAAoABQAFAAwAu/8DAAsAOf/9/+H/VgAEACsA9/8KAB0A3//8/8H/w/+3/zsAf/8OAAIA6f8nABEAIQD9/ysA9/9a/wwAXQDz/zMA+/8oAN7/JwAhADYAOwAZANj/gwA9/wIAqv8vACAAy/9QAP8AdP/z/woADwATAOL/KQDz/wAANAD4/xcAHwC3/z4Auf/0/y0ADwDR/0QA5v8OAB4AdwAFAO3/HAAcAPr/7//5/+n/CQAKABwA9v8IAIX/5P/u/04Awf8wAA0A4//e/w8Akv9m/xoAOwDZ/1EA7f87AIr/yP/2/w0A3/8qABsAv/8kAEgA9//x/zEACAADADgAKADp////s//F/3P/2f8VACUA+P8+ACAANgAHABUA+v9FAB0A8f8GACEA5//4/wsAHQAGAA8Awf8ZAAAANQAwADQARgA+ADAAv/8dAOz/PgBSAA4A1f8IAAIAo/8MAFQADQBm/w0ABAAEAN7/uf/n/zMAJACB//H/xAAAAC4A+/8AADQAAwBHABcAwf8iAEAAKQAxADgAwv8CAOP/SQBq/yYAAAC4/+f/+v/B/y8AMQA+AMEAEwDy/wwAGwC4/0EAGAALAPX/2P90/9X/Wf8UAC8ApP8/AKj/CwAgADUAGQDX/+v/BwAfACAADwDL/xEA/P/1/0wA9P8fANj/JwAaANQAEwANAA4AFAC+/wYABwBR/wUA9P9GAAEAEwDs//z/CQDl////tP/A/8r/TQBx/xYAAADz/x4AFQAjACAAKAAFANj/AABfAKb/MwD0/y0A3P/b/04ATgA1ACIA8/+EADj/x/+q/ygANACz/0cA+QBs/wMAMADs/x0Atv8dAOD/KgAtAA0AFgA5AMD/SADT//X/LgASANf/QQAFABMAEQB4ABIA+v/2/yUA3f/9/+j/GwAAAAMAIgD1//3/cP/w/+D/VADT/zMABQDo/+b/HAB7/3//GgAuANn/SgAAADYAfP/R//r/BQD7/zIAIACR/zMAPwD3/+L/KgAWAB4AMwAyAPH/FACw/8X/cf/w/wwAPQD4/z4AKAA8APf//f/+/zAAIgD3/xMAJQDX/+n/4f/g/xUAKwD4/0YA9f8oAE4AKgBGADYAJgC9/x0AAgA+AEUAAwAdAFYACAAAACMAUQDO/2f/DAAnAAQAqf+8//L/SQA5AIH/AQC+AAIALQDu/8b/OADi/zAAGADC/yAAQgAcADYAOQAQAO//7f9PAIP/OwAVAJv/5v8EALb/PgAlAEgAtAAUAPn/EgATAKr/MwAsAAoA4v+y/6H/5f9i/+X/HgDm/0cAg/8BAB4ALwAQAOr/9P82AB8AKAD5/83/KQDm/+n/SgDl/yUAwP8oACwA2QAfACEAEAAIAK//FQANAFH/CQDb/38AGQAnAPn/CQADANT/EgDI/8H/wv9NAHT/CgARAO7/BQAcACYA//80AOP/uP8WAFUA4f8YAMH/MwCc/x0AGAApAE4AIAD6/4cAVf8LAKv/MgAJAMH/TgDzAFP/5v8OABoA+f/m/0cA/f8dABoADAAdABoA2v9BANv/zv8fAB8A5P8/AA4ADAAYAHkA7f8JACYA0f/r/xAA5v/w/wgA1P8iAO//AQB9/93/1v9SAOL/LQD9/+3/1P8wAKP/fP8WACsA7v9YAA0AQwCD/7n/6P8XAPL/IgAkAM7/LgA3AAQA8v8yAAsABAAsABUA/f8bALb/w/9q/wAAIgAuAAIASgBQADIA//8sAO//OQAPAOT/+v8cAO7/CQASAOj/6v8eAA8AIAD9/x4ALgAlABUAPABLAK7/HgDK/ywAUQD4/w0ARgAsAK3/BgBcAMv/X/8UADAA/f/y/7X/4P9AACAAhf/c/7YA//8xABAA+v88AA0ATgAJAKn/MQBEADYAMgA1AO//HADV/1QAf//+/wMAof/i//3/nv+QABEAVwC+AOD/EQASAB0Awv9HAD0AEQDV/87/m//H/1//7f88AMP/QADA/xgA/v8pAA8A9P/n/xYAEgAsABUAz/8GAAAA7P9DAAkAGgC6/w0AKADQAA8ADgAcAAEAx/8XAPL/m/8QAOn/WAAGAB4A4//9/+L/9f8AANL/y//T/zYAaP/9/w4A//8/ACMAJAAVAG0A3P+x/w4AWgDw/1IAyf84ALr/vv89AGb/PQAWAAkAgwB0/w4ArP89ABsAuf88APAAYf/y/yIABQASAPz/DgDx/wAALgAKAB4AIgDd/zgA6P8RACwAFgDv/0YAEQALADAAgADp//7/HQD7//7/5v8RAAEADwDt/xsA7//+/3j/AQDZ/1gA4f8hABwA3//f/wwAi/9w/yYAGwDm/zcA8v9AAIb/3P/Q////AAA0ACAA5v9HAC0ACQDM/0AA9/8PACsAKgDY/y0At/+2/3H/0/8JABMAAgA2AC0AOgABAAEA7v8+ACQA6v8BADQA9v8TABQAyv/r/yQA9f9SAM7/FwA0ACcAGgAsADcAv/8UAOP/JgBfAOn/KABZAB0AFwAaAI0A+/9f/xMAJwD5/+X/xf/o/0MAJgCJ//T/uwAIACsABwDu/zwAKgA/ABMAnf8tADkALwBWADgA7P/2/9j/VwCR/wwAHADq/xcA/P/j/y4AOABJALAAFQAKABkAJQDE/zcAMgDy/9f/2f+U/87/Zv/r/zcAEwA0AHj//v8VADgAEwDo/xsANgAiACUAHwC8/w4AAADo/0kAFQAiANz/WAAuAN4AGAAFABEACQC2/wwADwCO/y0A1f9UAP//IwDy/w8A2//Z/xEA7v+t/6z/EwBg/xUA/f/9/zEAFAAfABEAKQDZ////BwBHAM3/FAANABcA7f+c/xMAIgAYABUAu/+LAIf/EgCn/yoAEwCv/0YA9ACW/wQAHADa/ygA6P8KAPn/9P8/APr/EwAiAL7/KQDc/yAALwAcAPz/TADt/w0AJQB9ABYA0f8SABgA9v/v/9z/9f/v/wQAEAAYAAAAh/8CAPn/OgDc/xcAEAAQAAYAMgB5/2n/HQAfAOL/OgDr/zEAkv/y/wgAGwAGABUAFQDh/yIANgAUAMf/NgD+/ygAGwADAAgA6v+V/7//Zv/z/ycAFAD1/6r/IwBDAAgAHQD0/xcAGADq/woAFwDe/+T/DAAoABIAGgACABoACgAaADEAKgBCAD4AEgDG/w4A1/8rAGMACQAFAOz/EQAQABsAGgAkAGr/FQAdAAAA3v/C/9P/GAD5/3f/AgC/AO7/LAAXALX/SAAUACsAGgCM/yMACQAwAAcAMQDh/zIA4P8vAIz/LgAFAOn/CAD8/73/GwAnABYAwgDq/yYAAgAcAJ3/KAAiAC0Axv/y/5z/6/9m//P/HQDf/zoAt/8fAAsAGQAeAMn//f8AABAALQAMAM//KwDm//X/UgD+/zwA7P8dACkA4gAFAPL/DgADAM//DwDu/5D/6v/O/zYA7v8iAP///P8CAND/BgDo/7n/n/8WAHD/CwAJAPb/LgAlADwALgAnAOT/yf/6/zkArP8mAAwAHQCt/ykADAArABwABgDR/4QAK/8QAKn/NgAaAML/RwD8AI3/8/8eAPf/GgDU/zYA+f/x/zAA+/8OACAAuP8aAPX/BwApABgA2P9MAAcAHQAeAHcAFgAqABcALgDp/+7/6f8TAB4ABwAUAPj/CgB//wYA9f9JAMr/LQAEAPz/4/81AIH/df8uAC4A3/9FAOD/MQCS/9j/pv8XAPD/JAAWANr/LAA8ABEA4/8xAAYAHgAtAB4A5//+/7H/1P9l/+3/GAAaAAQADQAoAEcAFQAPAPf/JgAWAOn/CwAOAOf/7f8cACYACgAcABEAFQAJACIAJQAyAD4ARAAtAMb/GAAMADcAXQAIAOT/4f8VABsAIgAiAC4AYf8NAB8AAQDe/7n/x/8aAP7/dv/3/8cAAwAoAOP/8/9NAP//OwD+/6v/EwAQACwACQArAOX/HwDi/zAAcf8qAAsAz////wAArv8hADAAIwC5AAMAEAANABkAof8xACQAUwDc//D/bf/e/1n/DwAkAL3/NQCB/w8AFQAcAB8A0P/w/wAAFAAhABUAyv84AP7/9v9QAAEAHQALAA8AIwDoAAAA+v8QAAsAxf8OAPT/Y/8JAO//NAD8/x4A7v/q//7/8v/9/+z/uv+7/wwAdf8oAAYA9P8lACgALAD//y0A1P/O/xQAPQD5/ycAAgAcAML/NQAnADsAFwAlAKf/hAAl/xsAqf8rABkAwP9EAP0AfP/z/w8A6/8XAOH/GgDp/+3/PgAJAAAAGgDI/zgA0v8PADIAFADG/0kABgAOAD0AdQAPAOn/BQBXAPH/AQD5//j/GAD5/0UA9P/0/3//+//r/0wA2f8zAAkA7//c/yMAh/9j/xcAJwDf/0kACAAzAIL/4//x/xcABAAeAB8AxP8mAEkAGADB/yQAAAAWACcAMQDh/wIAtf+x/2D/7/8fACAAAQAhACMARgAGABgA9f82ABsA8P8KABgA8v8JAAMAJAAHAA4ADgAoALP/KAAmAEAASQBHACAAwv8XAPr/SABYAAkA/f/o/wkA/f8YADIAHgBY/xAAIgDu/8n/u//p/yQACwB3/wAAxAD2/zEAEwDZ/04A9/9AAAsAmv8oAB0ANQADADQA1/8gAOT/KwBr/ygAMQDV/wAA/v/I/ysAKAAlAMUA//8YABMADQCh/zQAJgA2AOH/AABh/8T/Wv8PACgAz/81ALL/IAAeACMAGwDk////BgAeACQABgDS/xsA4/8JAFAAAQAyAPH/IgArAOUADQD5/xQACQDJ/wIAAgBi/wYA9f9GAPT/LwAAAPD/CADd//n/zP/C/7D/LgB2/xYABADo/ycADQAjACYAJADW/+j/AwBFAAAAHwDj/zgA5f/I/yMALQAgADYAtP+FAC3/BwCo/y4AIgC5/0AA/ABx/+j/DwAPACMA5/8tAP3/AAAwAPX/EgAeAMz/OQDk//P/LQARANL/UgDt/xkACgB0ABEA7/8HACwABwABAO3/6/8JAAMAJQAFAAkAg//4/+//VwDS/0AADQDh/+v/LwCg/3v/FgA0AN//RAD5/y8Akv/g//P/DQACACYAFQBn/zEARQD9/+L/LgAAAA0AAQArANf/EAC2/93/X//b/xMALAABADQAIAA+AAcAGgD3/zUAHQDX/xAAHwDW/wQA7v8jAPX/HQATABoA/P83ADkALgBfAEYAOAC9/x8Av/8+AFcAEAAT/wQABwCp/xQAPAAlAFv/EgAYAPj/5/+6/9H/MwAPAHX/+P/FAAEAIwD///P/TAAIADwACwCq/zgAMAApAP//MwD2/xMA4P86AHj/JQABAMz/8f/7/7v/FQA1ADAAwQAcALT/GQAeALX/PgAcACEA5//4/3X/0v9V/yIALQDP/zkApf8UABAAKwAfAMD//f8XAAQAKAAZAMj/JQDp/wYASgAWACAAzP8cADAA5QAFABMABgAKAM3/BAD+/0f/EgDn/0IAxv8RAP3/5/8bAOX/1f/K/8T/uv8rAHv/GgARAOj/JAAXACQAEwAuAN//xf8XAFcA5P8lAAMASgDf/43/PQA8AC8AGADi/4UANv8XAKn/MgAbAMD/PAADAWr/BQAeAAoAHADl/xIA8P8mABQAAwACAA4Awv85ANT/HAA6AB8A4/9GAAYAEAAXAHMA/P/p/wkAHQD3/wwA4v/8/wwA//8iAPX/+P9s/9//4P9OAM7/OAAFANv/6P8xAIj/fP8RAB0A2v9JAOP/OQB7/73/BwAfAOT/LQANANz/JgBSAAIA3f8kAPn/FwAwACAA3P8dALb/v/9f/9T/CQAwAAgAMgA6ADwACgARAPv/IQAXANT/AAAoAOL/7f/w/xsA+P8UAL3/OwD5/0EANAA3AEgAPgA0ALz/HQDr/zkARwAFAAkAGgABABgADwBPABsAWf8VACQA8f/d/7j/5/8tACkAff8NAMAADgAnAPf/9P9AABMAOQAVALb/KwA2ACoASAA9ANL/EQDt/0cAeP8eAAUAsf/u//r/w/8kADUAMQDAAAMABgAYABcAtP80ACgAEQDo//X/g//I/1D///8vAOj/QQCb////GgAmABIA2P8HAB4AEAAhAAkAy/8gAOn/8/9GAOH/NgC8/zMANwDdAP//JAAVAAkAuP8KAAYAX/8CAOn/aAD3/x8A6/8jAAoA0v/8/+T/zf+//0cAcP8QAP7/5/8kABIALgAmADgAAwC2/wMAVQCn/x8A6f87AOf/GQA1AC4AOAAGAAQAhQB7//z/p/8yABkA2/86AP0AWf+6/xEAIAAOAPr/JwDc//7/GgDx/xcAAQDN/zcAwv/k/zMAFADh/0IAAAAQAB8AbgDv//r/GgAcAAYAGQDo//b/AwAAABEA6P///37/3P/m/1EA6P83AAoA1//9/zkAmv9t/xsAKADZ/1IA6f84AIz/0v/k/xkA4f88ABEA0/8zAEYA+P/g/zYA9f8XADQAJADz/yQAuv/R/3f/8/8bAD4AAAAnAC0AMQAGAB4A9f8sACEA7v/v/0MA2v/0/woABQD2/xMA+/8wAOX/OQA1ADgANgA5ADwAv/8lAO//NwA/AP3/8f9DABEAy/8PAFwAAgBm/xQAHgD2/9T/wP/Y/yQAMgCC/wAAxgD9/y8A6f/q/zIA9f82AA8Aof8gADkALAAkADoA5/8WANb/PwCA/yMAAgC5/+T/+//K/wcAOABnAMAA5/8YABUAIADB/zAAJQAZAOz/5//H/9H/Xf8oAB4A7P9AAJf//f8aADsAHADl//H/KgAYABsADgDP/xwA+v/6/0YAAAAcALf/RAAwAOAACgAcAB0A+/+///v/AQBF/woA8/9HAPv/IAD0/+7/CwDf//3/rf/G/8P/LwBz/wEAFgDz/yMAGwAqAPX/QQDx/8b/EABZAPn/WAD6/xYAmP8oACAATQA4AAwADACLAD3/AgCn/y8ALwDV/zwA9QBk//T/IAD8/xEA3v8XANz/LAAAAP//AAAcANf/GgDh/9H/LwANAOX/TgDs/xQAJQB1AOf/AwAPAPz/8v8CAO7/+/8AAP7/DQDu//3/aP/s/9b/VADm/ysACgDh/9D/IgCT/2j/LgAuANj/VgDp/zgAgv/K/8T/FQACAC8AHADf/zYAPAADANH/JgDz/xsAHQA3ANn/RgCq/63/bv/r//v/QAAAADMANQA0AAQAIgDy/zUAIQDv/+D/RgDp//P/9f/3//n/IgD4/2IA1P8bADsAJQAsAEAAYgC9/yAA2f85AEIA7P8CAEUA+f+a/yAAZwDm/2P/EgARAOz/yP+//8//MQBEAH//+f/FABIAMgD3//T/OgD7/zgAHACX/wcARwAvADgAPwDm/w4A4f9PAID/IAATAKv/3//6/5f/BgAYAE8AsADf/wEAGQAVAML/QAA6ABkA1v/N/7j/vf9Y/93/OAAPAEAAlf8kADYAIwAOAP3/3v9BABoAHQAXANn/CQAfAAAARwDq/zQAwP87ACQA5QAXABwACQAAAL3/DwAYAHj/FgD5/0IADAAtAOj/8//x/+z/DgDZ/8//4v8dAGj/8/8UAPD/QwAoACQAMQAgAOn/6////0kA4/81AJD/EAC3//r/QgAIADYAEgDx/4sAfv8IAJv/NQAXAMb/UgDuAGr/4/8QABgAFAD5/zgA1v/y/yQABQAYADUA0/8wAOn/4v8qAAcA3P9FAPD/GwAzAH0A2v8HABcA+f/1/xIADwDk/xcA/f8JAPH/9f+G/wQA4v9TAPb/HQAKAOr/yP8MAKn/Vf81AEgA3v88AO3/OgCN//f/AAAhAO//QQArAPD/OwAzAAQA3P8tAAAACQAmACAA2P9GALj/vv9g//L/HAATAPv/LQA0AEAAAQAJAPX/IwAYAOD/4P8qAOv/BwAJAOn/AAAtAAoAXwDM/y0AOQAjACMARABXAMD/GADj/ygASgD8/xsAXAAhABMAIQBsAAsAav8YABwA9//h/7n/3v8vAFYAgf8NAMEA+v8iAAUAAgA8AAEANwAgAIj/JwA1ADkAJAA+AOD/CwDJ/1AAd/8SABkAwv/8/wUApv/y/04AKwCnAPP/3v8gACwAyf8nADMAAwDb//L/q//B/17/GgAjAA0AQQCS//7/GgA9AA8Az//2/1UADQAaACsA0f8KABEA4P9FAAsAFADI/1EAHgDhAC8A8v8TAA0Arv///w0AkP8CAOv/KwD1/x8A8////yMAuv8KAPb/x//D/woAX/8AAAcA8/8zABwAOgAmACgA9P/4/wsAMAAAAAwA+/8XAAIAEADx/yAAFgD8/+b/jQAs/wwApv8zABUA2/9FAPQAnP/4/xAA5/8sAOP/MgAhAO//NADv/xcAIQDI/yoA7v8dACsAEgD//0cACAAYACcAdwASAAsACAAkAPL/8f/s/ycAlP/4/wsAEgADAIH/AQABADwA7v8mAAMAJQDt/yAAgv+A/ykAGgDe/z0A/P80AJP/7v8OABkA//8gABUAsv8sADAAGQDz/yAA/v89ABsAKwD5/+j/lf/U/1n//v8rAAgA9v8BAB0APwAGACYA8v8wAB8A7f8UAB4A0//4//L/GwAIAC4A4/8iAAAAGQA2ACoAGgBAACwAu/8SAOX/JwBmABgAAQDv/wcA4v8gABUAJQBj/xYAJwAHANP/vf/M/xMAAAB1////vgD1/xoADgDv/0gAAQAsABAAkv8iAP7/JQAKADEA5/8eAPD/LwCS/zEACwDc/w4A+f/A/xMAIAAeALQA+P8AABMAFACg/zoAHQBGAL3/4P+d/+P/aP8EABoA0v8zAK//HAAJAB4AHwDM//r/8f8OAC8AFwDP/yEA7v8IAE8AAQAYAPP/JgAsAOYA/v/t/w8ADADN/xAAAwCR//b/8f8xAMz/KAAAAOD/DQDW/wUA4//C/7n/IAB0/xsABQD4/ywAIQAuADUALwD0/wsADQAxAMX/FwAVABwA7f8mAAEAJwAbAAsAw/+HACz/HACo/ysAGwDA/zwA8QCO/wUAJgDq/yMA3v8eAAsA7/8bAA8ABAAdAMX/JADl/xAALwAcAAkARgDx/yMAAwBuAA4ADAAQADMACwAAAOD/LwADAAUAHAAGAA4AfP/+//r/QwD8/zEAAADi/+7/NACT/3X/FwAmANv/QwAHAC0Ajf/k/6D/HQAMABQAGADI/ysAMwAUANP/IgAaAFoAIQAlAPr/+P+m/8L/Xv/5/w8ABgD4/wUALAA+ABAAJQD8/yUADADw/xkAEADc/+H/9/8fAAIAFwDz/ykABgAyACkANgA9AEQAIQC6/xcAzv84AFsAFQAcAPT/JwAOACMAIwApAFf/EQASAPP/4//B/9//KQAGAHr//P/CAOr/MgANAOr/SQAMACgAEACd/yUAAwAeAP7/MQD1/yMA5/8rAHz/HQAVAN7/+f/8/7f/FAAbABQAxQD7/xEAEwAWAKn/OAAtACgAv//A/3X/5f9c//3/LgDv/yEAlv8eACEAJwAgAMH/+P/7/xYAJwASAM7/IwDn/wUATQD+/zQA+v8hACYA6AAFAOb/EgAMAL//DwASAHb/+//h/zsAvv9CAP7/7v8OANb/AADk/8H/xP8TAHb/GQAGAOn/KwAsAFkALAAqAPH/7P8JADAAyv8ZAAoAJAC0/yoA6P8nABQAHQDn/4kAJv8ZAKb/LgAgANP/QgD8AIH//P8lAPr/JwD6/y8ACgDw/ywA6v8OACUA1v8hAPz/DAA0AA8A7f9DAAQAHAAuAGYACQAZAAwASQD9//H/1v8HAOb/AQARAPz/AAB///7/9/9LAOv/OAAAAOP/w/8kAI3/f/8oACEA3f9AAAgANgCP/+X//v8PAAYAKQAVALz/LgAvAAoA6/8nAPj/MQAfAC4A9v8AALH/3/9j/wcAGwAGAPL/HgAmAD8AEgAjAPX/LQAbAO3/FgAeAOD/+/8JACEABAAdABUAJQCu/yYAMgA+AE8AQwAtAL//IADW/z4AWgAOAN//AQAYAN3/JgAhAB0AVP8aAC0A8/+5/7v/3/8VAAUAeP8AAMUAAAAzABEA9P9OAAsAMQAJAJf/HwAIACMA/f8sAN//GwDs/ycAdv8UAD8Ay//1//r/x/8IADAALwDFAAIAFQAdAB0Aqv8wABoAKwDl/wIAdf/v/1X/CgAlANT/KACb/xgABwAnACAAxv/8//X/FAAgAA4Azv8fAOr/7/9QAAkAHgD3/x8AJgDoAAwABQAJAAkAy/8TAAEAc/8EAOz/NwC1/x0A/f/m/xMA7f/h/+3/wv+2/zYAcv8dAAQA7P8oABwAKgA0AB8Az//t/xgAMADp/ywAEgBAALD/IwAkADAAJgA5AN//hAAj/xoArf8vACAAs/89APkAbP/O/ykAAQAfAPX/JQDs//b/FwADAAUAGQDa/x8A5P8KAC8AAAD+/0QA9v8hAEcAZQAGAPf/jf9AAPP/CADe/xcAHAACAB0A/v8UAG//9//t/00A1/82AAQA4v/e/yQAk/99/xoAKwDb/0gACQA2AIH/5/8FACIADAAeABcAi/8xAEIAAQDQ/yAABAAnABcANADk/xUAt//I/1r/7v8JACQACgAcACUAMgAjABYA9/8nABgAHAAPACwA4f/x/9L/IwD6/xUACQAzAPn/OwAtACwAOQA/ACMAuf8fAPX/MABAAAcACQAEABoA2f8eADQAMABT/xUAGQDm/8b/t//S/zoAFgB3/wAAxwAHADcABQDk/0cACwAyAAoAj/8MACMAJAAXAC8A7f8VAOH/NgB3/xsACgDI//n//f/F/w8AMQAmAMoADQCy/wwADQC6/yoAHQD9/+f/4/+D/8L/S////ygA0v8uAMf/GAAkACIAEgDL//3/+/8mACoAJwDN/xkA4//4/0sABgA2AOH/KQAtAOUABQANAA0AAgDC//7/BgBw//r/3/9YAO7/HwABAPj/EADP/+z/7v/F/8T/OABv/w0ABQDY/yQAHAAnAAoAHwDm/+X/FABJAPH/LwAXADsA1/8jACYAPwAyACkA4/+JACr/DQCr/zcAEADZ/0AA/QBp/wYAGQAUABUA2v8vAPH/CQAnAPz/CwApANL/JQAFAA3/LwARAOn/RgAiACIALQBfAPr/9f8aAC0A/v8FANb/BAARAAwAEAAAAA0AdP/r/+f/SQDn/zQAAADj/wAAMACP/3r/JAAuAN3/UgAOADQAlv/k/+j/HQDc/ycAFwCL/yYAQAD7/+H/JQD7/yEAFwAsAOb/AQC7/8n/aP/a/xYANwAFADEAPQAxAAwADwD2/ykAHADg/wwAOADP//n//f///+3/FQCx/zAA7P8jADkAPABKAD4ARQC2/yEA3P8xADgACADy/ycAGQCP/xMARwArAFv/FQAhAO//6f+2/9P/FwAgAHf////LAAUALgAEAP//SAAdAC0AEwCk/xYAGwAlACcAMgDr/wgA6f88AHz/DAAEALn/8v/8/9v/CQAyAFMAxgD//wwAIwBEAMP/MgAoABMA3//x/4T/yf9P/woAOQDk/zkAof8TAC8AMQAXAMv/+v/6/yQAIgAbAM3/HQD4/+X/SQD0/yQAz/8zACUA5QAAAB4AEQADAM///v8IAFr/DgDl/0cA6f8ZAPr/+f8TAL3/3//t/83/2P8xAGz/DAAMAPH/JQAMAB4AGgA7APz/6v8cAEsAoP9PAAkAMQDi/xYAKAAvAD8ACwAEAIcAMv8EAKP/LgAeAN3/PgD+AGf/BwAgABAABQDk/yIA4v8CAPv/AQAJAAMA2P8zAPP/6f83ABwA+v9AAAAAFgAkAGYA/f/q/wgADwD5/+b/AQDt/wgAFgASANn/CAB2//X/3/9MAOT/NgAEAOD/6f8nAJz/ef8lAC8A2P9MAPb/NwB+/9T/wf8aAPv/IgAoALr/MgA7APP/3/8rAP3/DgAkAC4A6/8jALL/tf9t/+D/6v8yAP//MAAyADQAAgANAPD/JQAbAOr//f9hAMf/+P/+/xMA+v8aAPn/VADR/y8APwA2ADgASAA6AMP/HwDU/zEAJQADABUAMAARAP7/EgBRABMAVP8UABoA+//Z/7v/3P84ACwAfP/1/8oAAAAzAAkA6P9IAAYAOgASAKX/EwAwACsAPgA1APH/JwDk/0UAg/8oAAwAvf/z//n/pf8IADwAFgDHAN//BAAYABMAwv8tAAYADwDT//n/of/Q/0n/5f82AAkARAC7/xsAMAAoABgA2P/2/0kANAAZABYAxf8HAPn/+v9NAAUAOADH/0cAKgDnACEAGQAIAP//wv8EABMATP8KAOb/RgADACEA+/8qABkAzv/5//H/xf/g/z0Ac/8EABUA8P8uACQAGgAjAC0AAAD8/wcARQDZ/z4A9f8lAKT/HAAeADQAkgADAP7/iQA0//3/nf8wAC8A1/9CAPQAc/8EABEAGwAGAAIAOADh/+X/BAAFAA8AOQDP/y8ABQDE/y8ADgADAE0ACAAMABkAbADR/+X/EgARAP7/GgACAO//DgD+/wAA8////2n/AADl/04A+f8rAO//4P/9/ykAqv95/yUAPgDZ/00A9/82AI7/6P/8/xwA8/8rABcA0f8xADcA/v/x/ywA/f8RACAAIwDq/00Atf/D/2D/BQAfAAEA+P8pADIAQAAHAB0A9f8TABcA7f8JAD4A1f8AAPX/5v/x/xUABQAuAMP/KABBACoAIQA9ADsAvP8dANb/IwA2APn/FQBCABYA1P8bAFUABwBi/xYALwABAM3/xP/S/yUAPwB+/wAAxQAIAC8A8P8HADQAAwArAA4AnP8sAB0AIgAvADoA9f8XAPH/SQCG/xsAAgC4/+b//P+k//X/LgAkALUA6f/p/x8AIQDP/yMAKAAGANj//f+s/8D/Uf8bACcACgA9AK7/HgAYADkAFQDH//P/KwAGACgAHwDJ/yIAHwDu/0cABAASAMT/SAAhAOQAEgAJABcA8v/E/wcACgCJ/xIA8P8zAPv/IQDr//7/RQDR//v/9v/F/8z/OABs/xEAGwD1/zEAKAAgACcATADw//v/AABBAO3/XwDh/yMA1//z/ysAKgBWABYABwCQAEH/AQCl/ywAJwDP/zkA8AB0/wYAFgAMAAoACwAUAPH/+P8IAAkACwAXAND/LQAGAA0ALgAUAPf/PwD3/yAASwB3APX/8/8VAAIAAAAPAAAA+v8eAAgACwDy//r/j/8JAOX/UAD3/yoA+//m//3/QgCe/3b/MwA4ANP/OQD6/zYAkf/o/w8ANgAIAC0AMgB2/0oAMAAAAOX/MgAEACEAHwAuAOT/SgDB/7H/Zf/w/wAAGAACACAANwA3AAYACQDz/ywAHADq/87/LwDs/xUA+v8JAPH/FADn/0QABAAqADEAKAAoAEUAXADA/w8A2P8oADwAAgAhADcAIQDM/y0AXQAXAGL/GQAXAP7/0/+9/9j/NQBeAH3/+P/DAPz/KgD7/8P/QAAFACwAFQCU/xIARAAiAEMAOwD1/yEA8f9JAIP/AgAVAOL/7P/8/97//f88AJ3/rgDa/+P/FQAZAM//KAAjAAYA4v/t/53/yf9V//X/KQD6/z0Atf8jACIAFAAOAMr/EgBVABMAAwAiANb/KgAJAAsAQQAPADoAzP99ACoA4wAXAPL/DQD+/7b/+/8XAHr/BgDf/z8AAAAfAAEABwAwAMH/AQDk/8H/yf8YAGr/GQAFAPT/MgAaACoAHAApANv/AwAaAB8AEQAOAA8ADAD3/8r/5f8hABMABgDb/40AOP8IAJ7/IgATAMb/PQDwAJr/FAAZAOL/NwDF/xQAKQDz/yEACgAAABQAz/8fAPz/IQAwAAsA2f9CAAoAJAAgAHcAHgD5/xIAFwD8//H/8/8XABYABgAXAAQAAQCO//X/AwBBAP//KQD2/xcA4/8yAGT/ev8UACAA2v8pABMANwCI/+n//v8XAAEAFgAkAJL/MQAcABMA5/8eAPv/OQAjAB4AAADm/4z/yf9t//L/DAAHAOH/8f8VAEEACQAlAAAAHwAfAOr/FQD7//T/+v8ZABsACQAQABYAGQDt/xIAFwA7ADYALgAlALX/DwDy/x4AYwABAB8A8/8jAN7/JgAJACEAXP8XACQA6P/N/8b/2/8ZAAIAd/8bALcA7f8rABUA5/9LABUAHQAYAK3/HQD//ygAGQAvAOv/KgDu/yMAkf8VABMA3v/7//r/nv8LABgAFgDBAPT/5f8QABcAq/85AB8AAQCv/9L/af/m/2b/CwAmAP3/IAC+/xQADQAwABwAwf/m//j/GAAlACEAzv8oANf/BgBPAAEAHgDK/yIAIwDhAP3/9v8SAA0AzP8OAA0Ai//H/+z/LwDn/5EA9v/s/w4A+v8HAOL/vf+8/xcAaP8OAPz/5v8mABsAawAKACgA5P8BABIALgAGAA4AAAASAOX/FADa/xUAEgD+/8j/iwAl/w0AqP8uABQA1f9BAPMAkP/9/yMA9f8rAO3/KwAJAO3/MADk/w0AJADG/yUADQAhACwAFQD0/0MA8v8dADAAbAATAPz/IwAmABAA6//X/yUAAwAEAA4AAwD+/3z/CQAAAD0A8f8zAAEA4P/N/ygAkf99/xwAHADc/zYA+f81AJf/+P+k/y8A+/8qABQAl/8wACMACwD1/yYAFQAgABoAHwD8//P/lf/U/2j/EwAXAAcA8f8QAA0ARQAGACMA9f8zABYA8f8XAAUA1/8CABcAFQASACAAzP8jAAgAGwAwADwAKgA9AB0Av/8ZAOn/JABkAB4AAwD4/yoA8v8cABUAKgBU/xQAMAD3/8v/w//e/xIACQB//wcAvwDt/y0AIwD0/0cABQAvAAkArP8mAPv/JgAFACsA8P8mAOz/KwCK/0EADwDN/wMA+f+s/wkAJQAaAMUA9f/q/x4AFwC0/y8AGgAlALL/4v9x//r/Xv8LAC0A6v8dAKb/GgARAC4AHwDM//3/5/8VACkAEwDN/xYA5f8FAFIA//8jAOj/GwAnAOcAFAD9/xQAAwDO/xIABQDB/04A8/8rALv/LgD//+v/CQD5//L/5/+1/87/IABz/yQA+v/o/yQAIQA/ABQAHwDs/wQABwAtAND/EQD9/w4A7P8gAPP/IQAQAB4A5v+KADf/KgCn/y0AIADQ/zwA9wCD/xkASwD2/ycA5f8XABIA+v8vAPH/+/8aAN7/GAAGABcAMwATAPn/QAAKACMAFgBcABEA2//5/ysA9//8/9L/CAAgAAEAEAD5/wAAgP/9//f/PwDn/zAABADl/8n/MQCV/3X/FgAYANv/PwAIACQAiP/n/wUAKgADABwAHQCo/zAAJAAIANf/GgD9/yYAGQAOAAgA9/+0/8P/bP/3/xUAGgDm/xIANwA6AAsAJAD7/zMAIQDx/xgACwDp//f/AgAYAAkAEQD7/zUAtf8fAC8ANgA/AEUAIgCz/xsA7v8uAFQAGAAeAAAAIQAAACUAGAAjAEz/EgAkAAYA0v/B/9j/KgAJAHj/DADJAPr/OAANAP//TAALADUADQDG/xgA/v8tAA4ALgD7/x4A4/8lAIH/EwA+AN///f/6/7n/AwAhAB0A0gDu/xcAGAATALT/JwAOAAEAzv/Z/2//7/9N//3/MAD8/xwAnf8hAAwAMQATAM3/9//1/xAAJwAhAMv/KQDk/wEAUAD3/zYAz/8aABMA5gAOABQADAAGANT/EQAMAIz/+f/h/0sA8/82AAAA7/8GAOP/5P/1/8//vf8nAHj/GgDw/2kAJAAXAFMAFwAcAPT/CwAHACEA4f8WAAMAOQCm/yYAAwAbABQACgDg/4sANv8QAKj/LAAYANL/PAD5AHT///8qAP//IwDm/y8ABwD7/zEA7P/+/x4A3f8tAAoA+/8vAAgAEgBGAPD/JgAWAFcACADt/xUAHwDz/wAA1f8IAAcAAAAFAPz/BgCB//f/7v9CANj/NAADAOL/1f8rAJz/c/8eABoA2v9DAAcAMwCd/+z/7P8fAAEAMQAXAIH/KwAjAAEA6P8YAP//CwAkACEA6P///7r/0v9m//b/FwANAAkAHgAcADIABAAiAPf/LwAZAO7/DgAHAN7/5P8XAA8Ay/8NAAgAJwAGAB4AMQA8AEAAOAAtALj/IgDv/yUARQASAP3/EQAaAJb/MQAiADgAU/8UADsA7//N/77/1v8PABIAff8NAMoAFQA5AA0A8/9NABcALgASAKD/HQAQACYAFgAsAOz/GQDy/zEAd/8GAAUAzf/2//v/1v8AADIAOgDVAPD/pP8YABIAu/8iABUAKgDl/+//Nf/s/0v/8f8yAPD/IwCZ/yEAGAAvABQA0f/3//P/JwAgAB8A0v8jAOn/6f9QAAcAHgDZ/yUAKQDqAAwAAQAUAA4A1/8AAP//tv8DANT/NQDe/ycA/v/t/wUA7/+L//v/wP/T/xsAcf8eAAAA5/8pABIALgAtACAA0/8PABcAIgD//ywA9P8pALn/DAAXACgAEwAlAOX/iAA2/ysAp/8xACwAxP8+APwAbP8cACUAFAAPAPT/IAD6//T/JAD9/+3/FQDd/yMA+v8FADUABADq/0gA9v8jADUAUgACAO//8f8QAPb/0f/w////EQAKAAsA6/8EAHj/+//u/0EA0/8zAPf/3P/e/yIAn/9e/wAAIgDc/0EAGAA4AIX/9f/I/ycAAAAfADIAif8rACoA+//q/x0A9P8NABQAIQD2/xEAtf/A/23/4/8RACUA+/8fABUALgAbAAwA+P8rAB0A6P8PACgA2P8HAPr/BwD7/x0Ax/9MAOr/KQAzADQATABHACkAwv8dAPn/NwAmAA0ACwAYABUAnv8dACgAHABU/xMAGgDv/9P/wv/b/zwAIAB+/w0AxwAOADwAEgD4/00ADwAuAA4AqP8aAA4AJQA4ACwA8/8CANL/PwCA/xkADgDM//f/9v+x/wQAIwAXANUAyP8QABMAOwDE/xoADwD8/9z/6/+G/9P/Tf/z/zIA+/8xAMP/GQAfADAADQDW//f/IQA6ACIAJQDM/x8A/v/4/04A9/83ANP/JgAwAOoAGQAOAAoA///X/xMACwCf/wIA7v8qAAQALAD+/wEAAADi/+n/2v+8/9n/EgBr/xQADQDr/yMADgAnACQAKADY//v/EwAfALf/JwDX/zYAz/8fAB4AIwAtAAQA8/+NAG//BwCf/yUAHgDU/0UA+gBq////HQD1/wcA0f80APr/+//9//r/EQAdANr/GwAlAPT/NQADAOr/RQAKACMADABWANr/5/8cABAA8v/0//T//f8PABYABQDs/wAAe/8LAOL/RgDY/zEA9//l/+j/FgCr/3X/MgAlANz/RQACADwAmP/4/wwAIQD7/yQAMQDc/zIAHQDn//P/JAD4/xMAFgAmAN3/IgDA/8f/av/l/yoADAD9/ygAKwA3AAgAEQD3/xwAHQDv/w8AMgDE/wsABQD//9n/HwAPACoA4P8jADQAMgAyAEIAMAC6/xkA6f8fACAAAgAHACIAJwD+/zcAOgAgAF7/EQAlAPX/zv/E/+v/AwAsAIj/EgDJAB8AMQAKAOr/SAAGADgADACm/0cACgAsAEEANAD9/zMA5f9JAIr/CQACAL7/BAD7/6H/+f8vACMA1ADX//3/FgAVALf/HQAZABIA7v/0/8X/3f9a/wsAKgAAAEIAwv8WACUAOQAUAND//v8QAC0AGQAjAMj/HwD7/+T/VQAIAB8A0P8nACwA5AAwABAADQALANH/BgAIALT/DgD3/ycADAAhAPP/9/8jAOn/8v/9/8b/3v8lAHL/FQAUAPv/IAAiACcAJwArANj/DAARAB8A8v8sAM//EgCi/wMAHAAzADYAAAD5/4kAJf8LAJ7/MAAuAMv/PgD4AHP/CgAXAAwACAD5/yYA/v/z/w0ABQAIACUA3f8sAB0A+f8wABMA+P9RAPn/IwBGAF0ABQDp/xsA///+//j/DgDu/xIA/v8ZAN3/+f+H/wMA4/9BAOv/LwDx/+P/AABOAKL/dP8WADsA2P9CAP7/OgCU/+7/EgAoAAQAIQAnAM7/LwAjAOr/5f8qAAAAEQAcAC0A4/8uAL//wf9m//b/9v8nAP//HwAwADUACgAMAPn/MAAaAOn/o/9FAN//DQAPAAkADAATANz/HQAfACQAOAAwACQATAAyAMH/GgDW/y0AIgAKACYAMAAOAMH/FgAzAAwAXv8WACMA/v/H/8H/9f80ADMAg/8WAMUA+/80AAoA4f9FABcALgASAKD/JgAYACEANgAzAPL/DADc/1AAjf8UAAsA3P/p//j/w//x/ygA8//IALT/5v8eABwAyP8xAB4ABQDt//T/tP/O/1L/5/8uAPL/RACv/xkAGgAmABUA0v/2/y8AJAAYACEAy/8bAAMAAABNAAIALAC6/y8ALADkACMAFQAHAPn/xv8LAAYAlv8PAOT/NgAEACUA9f8uACkA1f8FAOj/wv/e/xAAdP/8/xgA9/88AB0AEAAdADAA4v/3//3/IACx/yIA5P8RAMP/GQAkABkAVAAHAPL/iwAl/woAmv80ACYA1P8/AO8Agv8NABYAAwAAAPT/MAD0/+n/7f8KAA8AIgDS/zAABwAGAC8ADAAMAD8A8v8hAAcAbgDw/+T/DQDx//z//f8GAOz/GAD6//z/6v/4/5f/AADj/1AA3v8tAOr/4f8DACQAsv9x/x4AOgDZ/0AA5/80AI//6/8QABIA9v8cABoAff87AA8A7f/r/ygAEwAKABwAIQDd/04AvP+7/2r/7f8NAA0A+v8eACIAQQAHAPv//f8rACAA3v8OABoA3v8JAAYA+P/6/wsAv/89ABAAIAA4ACcAEQBGAA4AwP8RAOn/KAAxAAAAKAAtACAAyP8YAFYACQBf/xIAJQADAMD/w//k/yoAWACC/wcAwQD4/xcA+P/s/0AADAAsABcAo/83AAgANABHADkA4f8VAO3/SwCQ/wEADgDk/9v/AQDB/+3/MgAOALQAs/8gAB4AGgDC/x8AJAACAAAA/v+P/8j/XP87AB4ADgBHAKb/HwAVACsAEAC//w0A+f8XABsAIQDG/yQAAAD6/0wACwATAK7/TAAoAOEAHwASAAsA+//T//T/AwCe/xEA+/8rAPb/GAD7/wEANADZ/xQA6/+4/9z/AgBp/9f/AgDv/y0AFgAwAA4AKQDU//3/DwAPAAgADAD9/xcA8P8IAPf/FwABAAsA0P+QADf/AgCq/ykAFADn/z4A7QCO/+z/CwDk/y8A2P8mACAA8P8yAO7/CQAoAOz/HwABACcAMwAMAOv/RADx/xkAFQB0ABcA8v8GAC4ACwDs/9v/+v8AAP//BQALAPP/gf8FABIAPAD8/y0A9/8JAOT/HACf/3n/FwAYANj/OAAFADgAjf/y/wYAJwDr/yYAFwCf/ysAEwAWAN//JQD8/xEADwAOAP7/4P+R/9H/bv8CABoAAADn/wYAFgA+AP//LQAAACoAIQDh/xcAAwDZ//H/CwAYAAkAGAARABwAzf8TAC8ANQAwACgAHwDH/wIA4v8LAGQAFgAAAPH/GAAOACoADAAbAFr/FAA1AP3/3f/E/93/JwAIAHj/BQDDAOP/IQAeAO//PwAEABAADwCm/y8ABAAuAAAALgDq/ycA7P8aAIr/IQAYAN3/AgD+/7P/BQAlABAAwgD0/xkAEQAcAK//NQASAAoAbf/f/4T/5f9k//z/IQADABkAs/8UAP7/NgAZAMP/9v/t/w8AKgAhANH/FQDX//f/VAAEABsAy/8XACsA3QD2/+f/HAAOANj/CAAZALf/AgDv/yAAvP8hAPL/7f8HAOf/AwD1/7j/xv8bAHn/HgDz//H/LgAaACoAFQArAND/CwALACMABQALAP//DQDt/97/6v8UAA8AGQDN/4oAQf8MAKv/KQAPANb/OgDqAHX/AAAsAOH/NgCr/w8AFgD1/yAAAgD9/xcA1f8bAPT/EAAxAAUAz/9JAAQAHgAFAGcAFQDe//n/FwAEAO7/4P8IAAoABAAJAP7/AACG//j/8P84APr/KwAAAND/3f8nAIj/bv8RAB8A1v82AAgAMACF/+H/b/8SAAUAGgAdAIz/LQASABYA4v8dAB8AHQAbABUA+v/t/53/yP9m//r/BgAUANn/DAA0ADIACgAsAAEAHgAgAPP/DQAEAPH/CgANABMABgAbAB8AJgD//yEAHAA3AD4ANgAcALz/FAD1/xMAXwAwAC0A/f8hABQAJwARADgATv8QACcA9P/k/8L/yv8kAAAAdv/8/8MA9P84ABoA+/9HAAcADAAOAKr/FQAAACsAEQAwAOn/GwDu/yAAlP8aAPz/6P/1/wAApv8BABwAFADIAOP/1P8pAA8Au/8oAAYAGgCx/9D/bv/c/1L/6/8hAAMADwCl/yQAEAA4AA0Axv/o//L/DAAgACYA1P8mAN7/8v9RAAIAKgDe/xoAIADhAAUA9v8IAAkA0P8IABMAy//w/+P/PwD3/yMA+v/r/w4A7//9//f/u//w/x0Aff8aAOf/3f8pABoAKgAlAB4A4P8UAAoAHgACAAwA/v8cAOP/GwDx/xoAAwAMANv/jgBr/wAArf8oAA8A0/84APAAjf8JACQA7v83ANX/LgAaAPT/IgD4/wQAHgDz/xEAAAAAADMACADt/0EABAAgAB8AUgAQAPr/EgAjAOj/7P/o/wYABQAAAAQA/f8AAIH/AAD//zkA9f8yAAcA3f/e/ywAlf91/xQAFwDZ/z0A4P8vAJX/7/8GABYA8P86AB0Amf8rABgADgDu/yUA/f8LABMAEAAiAPL/sf/I/2r/AAAmAAsA6/8YABgANgAHAB0A+/8vAB4A+f8QAAwA0P/0////BgDw/x4A4f8lAHX/FAAiADkAMwBHACwAu/8eAPj/LgBSABoACwAJAB4AAAApABkAMQBJ/xUATwD6/+T/uf/a/yMABQB7/xEA0AACADUAEwAIAEkACwAXAAgApP8YAPn/LgAUAC0A8/8UAPL/JQCJ/yQAQwDh/wwA9v/V/wEAIwAoANMA5v8XADQAFgC3/xwADAAHAMz/5/9r/93/Tv8CACgACwAMAKT/GQAfAD8AEwDM/+//5f8LACIAFgDN/ycA4f/t/1YA+v8TAML/GwAjAOUABAASAAQACADW/wUAFwC2/wAA5/8uAOP/GgD9/+z/AADt/9///P++/9T/FwCC/yIA6//u/yUAHgAtAAwAJgDV/xwAEQALAMn/IQDy/xAA6v8GAPT/FQAOABYA5f+RAEv/AgCr/zEAIgDE/zMA+AB2/woAGQDx/ycA5P8aAAIA8v8RAO7/IQAfANz/FwDr/xIAMgAIAAcARQD3/yQANABCAAoA5/8SAAcA7v/V/+v/GAAUAAIABQD2//z/ev8CAO7/PgDa/ysA/v/g/97/MACh/3j/BwAjANj/QwANADQAkP/+/8//FwD9/xwAJgDY/y8AEAABALf/FwD1/xsAJQAfADQAAAC5/7//cP/y/xwADQDc/xUAGwAxAAEAGQDx/yoAFQDq/xIAHADg/woA/P8NAAQA//8XAEcADAAMACwANwAuAEIAIgCz/xkA8P8ZADoADQAiABAAFgDk/yQAGwAZAFL/FAAyAOf/9P+7/9D/FwAOAH//DgDQAAEANgAKAP//SgAbABwAEQCf/zkACgArACcAMQD0/xcA+/87AIr//v8LANL////x/7b//f8ZAAwA1QDP/3T/FwAFAMT/IAA1AAkA0P/X/1H/8v9K/+n/KAALACMAlv8cAA8AOQANAM7/9P8TACEAKgAIAM//KQDi/9f/VwAGACQA0f8kAAkA5gACAAoABwADANv/DQAQANL/BQDZ/y0A+P8cAAEA9f8MAPD/6P/7/8X/4/8UAHL/BgDu/+r/KgAYADIAIgAoANz/HQAHAAAA+P8TAOX/HwC3/xcACgAgABkAAQD2/4wAUP/8/6n/OwAaANz/PAD9AHz/AwANAPn/GQDm/ykA/f/u/xkA8f8CABcA4f8bAAYAAgA3AAYA7f9DAPv/HQAaAD0A///v/xUACwDv/+v//P/1/wUABwD+//r/+f99/xoA7v88APH/LQD+/+L/6f8vAL3/dv8qABUA2f9HAAMANACe//j/EwAPAOz/HQApAOb/IAATAAUA6P8eAPb/CAAOABoACAADAMH/yv9q/wQAHAAYAPT/HQAOADkABwAQAPP/LwAXAPj/CgAFANj/DwAJAPv/9v8LAIT/JwDl/xEAMQA5ACUAPwAmAMX/GQDt/x0AJwAJABIAGAAkAM7/jwAlABAAW/8SACgA/v/o/73/0v83ABoAgf8RANAAJAA0AA8ABgBJABIAGwAVAKL/EQD//y0AMgAsAO3/BwDv/0MAjP9KABIAz//x//X/w//5/yQAGgDeAMX/FgAaAD8Awf8fAD8AEwDX/+b/dv/b/0z/4v8sAAoAMgCZ/xYACAA8AA4AzP/4//f/IAAYAAwAzf8gAO//lP9ZAAEAGwDV/x8ALADoAAoABQAAAAMA3f/y/wkA5f8KAOP/IQDg/xAA+P/z/xEA0//Y/93/u//r/xUAe/8mAP///P8sABIAJgAJADQA0P8bAAYACQB4/xkA3P8TAM//AwAZABYAHgAPAPL/igBi/woApf8cAC0Az/84APgAZv8KABAACAD7/+7/IAD8//f/9//9//z/FgDj/yIA7v8UADQA/P/l/0MA//8UABsASwALAOf/+/8EAPP/8v/j/wAAEgAEAAQA3//8/4j/CQDl/0AA0/8sAPb/2P/+/xgAvf9v/yUAHQDW/0cA7f8qAIz/7v8WACgABwATAEEA2f8pAAwAFADq/x0A/P8OAAwAGADr/y4Av/+8/23/6P8MABQA+v8XACMALgAOAAkA9f82ACAA/P8AACYA2/8iAAAAAAAGACEAFQAtABgAEgA1ADcAOQBHACcAsv8YAOT/JAALAAgAIwAVACgA/v8zAB0AAwBX/xIAHgDx/97/wf/V/ygAJACC/xUAygAaADoABQD0/0gAIgAZAAkApv8XAAcAIwA2ADAA8v83APT/RwCT/wEADwDA/wAA+v+8//3/IwATAN8Avf/0/xsACADG/xYADwAAAMr/4P/W/9v/T//l/yIABwA8AMX/GAApAEEAEgDD//X/BQAhAIsAJgDM/z8A7v/V/1cA/P8rAMP/MAA2AN8AFAACAAUABADR//7/EwDb/wgA7P8nAAcADwDw//j/EgDz/wEAAgC5/+z/FwB///f/EAD4/ycAGAAmABUANwDU/xgABQAVAOv/HADO/zEAdP/P/ygAJgAxAAQA+/+GAFT/+/+Y/zIAFwDk/zoA8gBm//n/DgD2//v/7P8fAPL/7//8//7/CAAnAOD/IAD2//n/MQAHAPj/TAAHABIA/v9WAPT/6v8EAAkA9v8AAA0A9v8MAPr//f/Y//v/j/8MAPH/OQD4/y0A2v/k////NgDC/33/PAAqANr/QwDs/zEAmv/2/yEAFAD5/xUAIwDT/ykACwDr/+f/IwD//wkABwAYANz/NQC+/7//Xf/6/xoAFQD9/ycAJAAaAAkA3//w/xwAHQDn/xIACgDY/xQA/P/r/+b/HwDn/xcAHwAWADAANwAhAEEAMQC1/xUA0v8qABsABwD5/yUAJgD6/yQALgDW/13/FAAyAPb/2f/I/9j/MgAmAIb/AgDJACQAHQAIAOX/OwAPACIADgCZ/0AA+P8oAEYAMgDz/woA8P9IAJD/GQASAOH/+v/6/7//9/8rAAMAwQDF/x8AFAAWAMD/GgAcAAMA+//o/6n/zP9c/+j/DwAeAEsAsP8YABIANgAnAMv/9f8hAAMADAAZAMf/FgADAMf/VAAeAAwA0P8OACwA4gAVABQABwACANj//v8KAOv/DQAgAB4AAAASAPX/+v82AOv/CwAAAK7/6f8NAHf/BgAcAO7/MwAcABwAJAAvAM//BwAHACIAv/8kAN3/EQDR/6z/KAAbACsA8P/w/4sArf/1/5j/OAAoALr/NADpAHP///8KABUA5//w/xwA+v/s/x0ABAAUABkAy/8jAPH/DwApABsA6f9LAO7/LAA3AHIACADo/xsA8P/2/wAAGADx/xwAFwAFANX/+/+R//n/3/88AOP/LwAKAOf/8P8cALP/aP8sAEAA2P86AOv/MACC/+3/GAAhABAAGAAvAOP/PAAPAPT/6f8nAPz/BwASACEA4f8vAML/s/9e/+3/BQArAP7/HwA9ADAAAQD6//f/EwAeAOT///8uANf/HgD9//f/AgAZACEALQAUABsAPAA6AAwATAAjALX/+f/Z/x8AJAD9/yMAIAAfABgAHAAwAA8AXv8UACkA9f/o/97/4/8gAFQAff8IALkABAAnABIA2v8+AAUAFgAVAKX/LQAEADIAQwA1AOv/GQDq/04Af/8PAB8A9f/9//3/lP/z/yUACwC+AKr/KwAYABcAuv8TABIAAwDz//f/tP/U/23//P8XAA8ARgCj/ysABAArABoAvP/4/wsAEgAMACMAz/80AOn/2v9GAAwAMwDJ/zYANgDZAA8A+f8AAPv/zv/+////xP8KAA0ANwD//xIA3v8tADwA5////wEAxP/Z/xgAcP8ZAP3/8f82ABEAJAAUADUA8f8QAB8AEgDv//3/8v8PAPD/3P8GAAsACgAcAOn/iwAy/xEArv8lAB8Axv9X/+QAo/8FABQA6P8/AM//IgALAP3/MADr/wAAFgDr/xsA3/8QADgADADj/0UABwAjAB8AagAZAOT/6f8BAP7/+P/T/wYACgD6/wsACQD0/4L//P8EADkA+/8lAPH/BwDV/ysAef9s/wYAEQDh/zYA8f8uAJ7/9P/M/xYABQAaABQA4/8zAAQADQDp/yoA9v8OABIAHAD9/93/pP+1/2n/9v8XAPv/8P8PABwALAD9/y0A/f8ZABcA9v8LAAAA1v8CABIAEQAHABEA6/8hAAsAFAAeADQANwA/ABMAqv8FAPT/9v9fAAwAIQD8/yUA4v8aAA8AHwBl/wgAGQDx/9//wP/E/xoAAgB4/+n/vgD1/y8AEADv/0MACAAGAB0Arv8ZAPb/MAAcACsA5f8lAOv/HACO/yEADQDj/wIA/f+q/wAACQAMAMgA4/8QABIAJQC7/y4AEAD//4T/1P9x/9j/YP/1/yEAAwAaAMH/IwAUACoADADI//D/5/8LACoAKgDR/yMA4P8AAFkABQAzAOH/HgAgANwABwDs/wwA/P/S/xoALgDv//3/3/9DAO7/KQD1//H/DADv/wUA7v/K/9P/EQCB//D/9v/3/zkAFQAmACEAKQD1/yIAFQAXAMX/AAACACUA6//x//7/CgAKAAcAzf+RADj/AACr/ycAHADc/0AA5gCV/wkABwDg/z4A1v8kABMA7f8hAO//CgAfAOj/JADz/wUANQABANn/TQD2/w8AGgBcABoA4/8AABEA9P/o/9L/EQAAAAAA/P8JAPj/ev8IAPj/PADv/y0A9v/g/9L/NQCi/4H/JAAYANv/PAAGAEQAmv/4/5P/CwD2/0QAEQCB/yoAEAAZAPf/HwAvABMAFQAWABoA5P+0/7n/af/z/yQABgD3/wwAJwAwAAMAKAD9/yMAGQDr/xgAAQDW/wkAEgADAPX/HgALACQAwP8VACcAOAAuAEQAJACv/yMA8//2/10ACQAYAAMAIADd/yAADAAmAFr/EwAhAP7/2f+//93/HgAKAH//AgC4APT/MAAgAAYAOwAWACAAFwCo/xoABQAyAA4ALQDp/yMA+/8gAIn/DQAQAOr/CAD+/9b/+v8kACIAtwDl/w0ARgAaALr/IQAWABsAh//x/1r/3f9a/+7/LQAFAAoAo/8ZAAcAOgAHAM3//P/3/w8AJgAXANL/IgDu//f/VwABABIA6/8jAB0A4AANAPD/DQACAMT/GAAUANv/8f/s/yMA5v8eAPb/7P8DAOr/3v///7v/8P8bAJH/HgDo//n/NwAjACcAFQAiANr/HQAMAAcA6f8MAAAAEwCv//v/+f8OAAsACADn/5QAKP8AAK7/IAAgAMn/PQDtAFL/GwAdAPb/PQDV/wQADADv/yQA6//x/xkA6/8aAOn/GgA0APr/xP9DAAAAKwAvAEkAEQDn/wcAFwDn/8//1P/2/wQABwD+/////f9+/+//9P8+AAoAKQD8/9P/zP8aAJz/bf///yEA1f9CAAYAMgCT//T/wf8AAAsAGgAbAOH/LgAMAAwA2f8PAPH/CgAKABEACQD5/7P/u/9l/+T/DAALAOP/CwAPAB4AAwAeAP7/NwAXAOr/EwATAMf/CAAEAAgAAQAgAAcAPQC1/xQAJAA5AC8APQApAM3/GQAFAPH/VQATACoACAAfAMr/JAAgAB8AVP8VADMA7v/y/7v/0f8fAAAAfP8TAMEA/f8+ABcACwBKABAABQAVALX/IQD9/zMAHwA5AO3/GAACADAAjf8DAEUA5P8AAPz/sf/+/xkABwDBAN3/4/8dAA8Auf8jABoACgDB/+L/R//j/1H/9v8gABkAFACi/x4ACAA6AAQAy//t/wkAJQAWACIA1f8XAO3/6/9YAAwAHgDO/xIAHQDhAAMACgAHAAUA2f8LABgAw//y/+3/KwAIABwA+//z/wsA7v/7/wIAvv/U/xAAlf8PAOT/5v8yAB0AKAAWACgA2/8nAAUAAADy/xgABwAUAML/DwABAAcAHAANAO7/mgCG//j/sf8kABIA0/88APYAi/8EAA0A/P8xANL/KQATAO3/HwDw/wEAHADu/x8A4/8YADUABQDZ/0QA+/8dAB8ASgAFAPf/DgAFAOz/3//s//3/DQAQAAAABQD6/4P/EwDx/z4A+P8nAPj/2//W/ykAs/9y/wYAHQDb/zsAGAA6AJ7/8P8fABAA/f8aABQA6f8nAAoAEADh/xUA+v8IADQAHwA2AAAAuv/G/2T/6/8WAAwA7f8UACUAJAAAABkA8f8mABsA9f8MAAcAx/8HAP3/AwD1/wYA3P8nANv/+f8xAEEALQBCACUAyv8VAO7/BQBKABIAEQATAB0A0v8rACoAGwBa/xcANwD5/9r/uv/g/zUAEQCC/xsAygAoADAADAARAFAACgAYABQAtv8WAAAAMwAnADsA8v8XAPj/RgCO/xwABQDV/wYA8v+5//z/HwAZAM8A3f+z/ywACgDH/xoAEAAEANH/+/9z/+H/Pf/s/ykAIAAXAIz/IwARAEIACgDR/+3/9P8QACQADADU/zcA4//v/1kA//8VANf/KAAgANkADAAOAAMA/v/Y/wcAEgDi/wwAyf8iAOz/HQADAOz/BQDi/+7/DwC7/+b/DQCS/xkA7//2/ywAIAAlAAMALADZ/ysAAgD7/8P/GQD3/xgA7v8BABEAGAATAAcA+v+RAGD//v+q/4oAHQDS/zMA8ACS/xEAGADu/xUA7f8QAAYA7f8QAP7/+P8SAOf/JgD4/wsAMwAHAN7/RAD+/yUAGgA/AAgA7P8BAP7/9//o/+n/BAAMAAkAAQD9//f/gv8tAOv/OQDt/ycA+f/Y//v/EgC+/2v/LgAUANb/RQDw/ysAof8EACAAFwAGABEAIQDg/yYABgACAOT/GQDz/xwABwAYAPb/EQDD/8n/av8EABcAEwDy/xUAEwAsAAQAEQD4/yMAGwDp/wwADADN/xIACAAEAP3/IwDG/yoADAAUADEANwArAEcAJgDA/w0A1P8SACgADwATABAALQDb/yYAIAAYAGD/FAAXAPL/5f/B/9L/HwAYAH7/IAC0AAsANgAVAAMASgATAAMACwCr/xsA/P8zACkAMQDq/w0A+f9EAIv/BAALANf/AAD6/6f//f8dAAsA3gDE/+f/HQBDAMP/JAAgAP//yf/f/3b/1f9F/9v/KAASADMAov8gABMANQAGAMn/9v/3/x0AFAAlANL/MwDf/9r/VAD4/yUA0f8qABwA3gAJAA8ABAD0/9L//P8WAOv/AADd/ykA/f8eAPL/9/8CAAkA7v8AAMX/+P///4H/AADz//H/MAAWACoAFQAnAO3/JAD///X/v/8VAAEAEwCk//f/HgANABkAAQD//4UAMv/x/6b/JgAeAN7/NQD2AGP/BgAQAAAABgDs/x8A+v/y/ygA8f8UACAA2v8YAPn/+/8yAAsA2v9BAAMAHwAoADwA8P/p/wQAAwD///H/CADz/wgACwD0//7/9f+K/xkA7f83APT/IwD8/97/7f8zAMD/cv8RABgA2f9GAPv/JQCr//j/CQAPAP3/CwAlAPD/JgAJAAYA2v8eAAIABQD7/xoA+f/5/8X/xf9m/woAJQAAAP3/EAAdADIADAAIAPf/KwAaAOz/CQAUAMv/FgAPAAUA8/8bAOL/HAAPAAAAOQA1ACcAOwAjALn/FgDt/xQAHwAMACUAEwAoAAAAJQAnAAgAYv8UACQA8f/n/8L/1v8uABsAiv8YAMIAIQAlAAMA1/9BAAcAKwAUAK7/IAD+/zQAMQAsAPD/PQDw/0gAi/8nAAcAxv8JAPj/nP/5/zAAGwDgAM//EQASAAUAzP8XABoAEADq/+H/nv/U/03/6f8UACsATgCQ/xQAJgA8AAwAxf/2//j/DAAGAA4A1f83AO7/9/9aAAMAFQDe/yUAIwDpAAIAAgAHAAAA3P8OABAA7f8NAAkAFwDm/wsA7P/x/xcA0f/l/wQAwP/p/wYAcv8XAPb/+/8zACQAIQAPACIA3P8hAA0ADQCt/x4A+v8PAJf/yP8mAAoAGgAMAP//fwCF/wcApv8pADkA3f80AO8Ag/8GAAQAEgDs//j/DADw/+v/CQD2/wAAHwDm/ykA3P8PACkAPwDf/0YAAQAkACEATwAKANH/8P/4/////P8BAPf/EgANAAAAvf/4/4b/5//s/zEA4P8sADEA5P/t/xQAx/9u/ysALgDY/z8A6P8tAJr/7/8YABgAEgAHABcA2/8wAAYAAwDQ/xgAAQAGAAAAGwDb/yoAt/+w/2X/9P8TAAgA+f8GAAoAOAALAPz/9v8kABEA/P8FAAMA0P/8/wAAAAD8/xEAAgAaAO7/CQA/ACwALwBKAC8AvP8LANL/GwAYAAEAKQAfADAA/v8SABQA9v9a/xQAJgD9/97/x//I/yoAHwCB/w4AugAjACcABADK/y0ABgA0AA8ArP8aAPD/LgA1ACoA5/8JAOr/SQCG/wwADAC8/xQA+P/H//v/OAAEANAAy/8KABsAFQDI/xgACgD+//P/5v+k/9P/Wf/g/woABgBVAMD/FAAVADQA8/+//+j/FAAbAP7/HgDS/ywA+v8BAEoA9/8mANn/FQAsANkADQDs/wUA9f/T/97/GgDr/xMAMwAkAA8ACAD6//L/FgDr/wMACwCn/+n/DABz/wAAAQAAABcAIgAhAAMAGwDY////GgATAOz/JAAEABkAmf/q/yAADAAiAAEA+v+FADD/8f+h/ysAEQDh/zkA2wB+////DwAaAOn/5v8gAOX/8v8XAAIAEAAWAMz/FwDx/xwAMgAHANX/SQAMACgAIwBnAN3/4v8YAAAAAAD0////w/8YABMABADp//n/kP/u/+j/MgDi/ykAFgDf/+j/KgC4/2z/MABNAN//NADm/y8AlP/7/xAAFgAIAA4AEgDY/zcADAACAOX/LAAEAPn/9P8MANv/FwC0/6v/ZP/e/xQADAALABgAJQA9AAAA5f///ycAFwDr/wUAGADH/yYA7//w/9r/GQDw/xEADgAVAD4ALwAJAEcAMQCv/+z/6v8ZACwABwAJABkAIgD//xQAIQD+/27/DAALAPz/y//H/8X/IQBQAIj//f+xABsAIwD4/87/NwDi/zEAFwCq/xsA5/8wADIAMwDz/xkA8P9IAGz/GgATAOH//f8AALb/8P81AAIAxQDO/wUAGwAXALn/GQAFAAoA9P/k/2H/4v9t/+//Vv8RAFMAif8jAA0AJQAEAL//4P/1//7/FwAMAMf/LwDp/wMANgAIAAgA0/8eABoA3QAjAAMABQDl/9D/9/8CAPf/FAAsAC0AFAAbAO3/AAAwAO//DADy/9L/v/8RAHD/3P/z////LwAZAB8AKgAsAPz/EACHABMA0P8cAAAAEwCn//j/DAAMABIADQDm/4kAMv/9/6D/JwAvAN3/pv/aAJT/FQAJANz/SwDb/ykAAADu/yYA9f8JACYA2v8eAO//DwAwAAUA5v9GAPD/HQASAGUAGgDq/wgACwANANz/0/8DAPv//v/1/xUA7/97//j/BgA2AOD/GAD4//n/7/8YAJj/gv8MACAA3f84ABcATwCW/+//6f8IAPb/MAAXAIH/JwAZABwA5/9HAAEAAgAMAA8A/f/c/6//uf9l/+b/FAAEAP3/FgAeAEMA9v8vAP3/IgABAPH/EwAFAMP//f8RAP//7P8ZANn/DwCY/wQALQA8ACQASwAmAKH/GwD1/xMAYwAbABoA+f8fAAgAHQAOABgAbv8HACcAAADN/7z/vv8bAAEAev/7/8EA7P8kABgA3P85AP//GQAiAKv/FwD9/zUAIwAnAN//MgDg/ykAi/8WAAoAyv8UAP//xv/7/xcAIwDCAN3//v8hABUAsf8uABsADACK/+P/nP/f/2f/5v8fABgAFgDX/w0ACgA6ACMAw//v/+v/DgAfABgA1P8cAPH/+/9PAAYAHQDd/x4AHQDZAAEA6/8RAPf/yv8SABQA1//8/93/IADx/yIAAAD6/woA3P/y/+j/yv/Q/xIAgv8ZAPH/9v81ABkAIAATAB4A9P8eADIACwDG/w4A9f8QAK//HQD//wQACAANANj/hwA9////pP8nADUAwf8pAOQApf8PAA8A6P9CANv/DgALAPD/JwDy/wEAFgDZ/xIA7f8VADEA+v/X/zkA8/8fABgAXAATAN7/DQAGAOP/yf/w/w0ABwD0//n/AQDy/23/9f8AAC8A1v8gAPH/4f/b/x0Anv9v/woAGQDc/zkACAAdAKD/9v9O/wYAAQAYABgA6f8nAAoAEQDj/zsAHwARAAoAGwAOAOv/sP/I/23/6/8aAPz/+P8GACMAQAADACwAAgAnACIA6f8WABEAzf8hAA4ABwAEAB8A2P85AAYAFgApADIAIwA9ACAAuf8lAOj/DABhABgAMgAEACMACwAXAB0AJwBX/xAAGADu/9L/vv+2/yoAAAB0/w8ArQDm/yUAGgDP/z8AGAABAB4Asf8bAOP/LAAgACcA8f8sAPv/MwCI/w8ADQDY//H/+/9W//b/EQABAL4A1P/J/yIAGQCy/yIACQAaAJP/4f+G/+D/W//r/yYAJQANAJ3/GgAMADkADwDC////EQAlAB0AGgDT/yEA2P/x/1IABQAoAOb/HgAZANsA+v8AAAYA///V/wMAGgDk/9v/2v8nAPf/IgDr//D/AwDg/xAA8P/b/+//AQCA//f/8//w/zoAHAAhABUALgDp/yIAGQALAOH/DQD3/xMAzP8FAAIADwAVAPz/5v+NAIT/9v+f/ycAHgDt/0QA2gCO/wwAHgDw/0YA3v8gAAMA6/8qAPH/DAAkANb/CAD1/xsANwAAANH/RwAAAB8ANQBKAA4A7P8HABcA6f/c/+P//f8FAAAA/P8JAPX/eP8PAOf/LwDu/yIA+v/q/9z/KQCR/5T/DQAfANP/MwAPAAsAov/0/wwACgAMABUAEAD3/zEADQARAPb/LgD0/wUAEwAbAA8A8P+1/73/av/s/yIAAAABABEAFgA4AAIAJQD1/yIAGgDj/xkACADA/wcA9/8CAAEAGgDg/yAAbP8ZAC8ANwAiAEQAJQDI/xUA9//5/1gACgAkABcAJAAAACkAJQALAFz/GAAyAP3/1P+8/8X/JQAOAH7/CwCyAP//JgALAOz/UQAXABEAFwCs/x8ABAA7ACYAKgDn/ygA9P9QAJr/CAA8AL7/+//w/7P/AAAWABUAzQDn/wQAMQAPALv/FAASAA4As//q/2f/2f9P//T/IgAdAA0AlP8VAAYAQgARALj//P/0/xEAJwARANf/HQDs/9T/VQAGABUA3P8cABMA2AAUAOf/CgD6/8L/AAAVAM3//f/s/x8A7/8dAPH/8P8WAOj//P/2/9r/2f8NAI//FgDq//H/OgAeAB4ADAA0ANf/HgANAAoAsf8UAAIAEAC8/xkACwAHAAoADgDt/5UANf/6/7P/FwATANH/MgDRAKf/BwARAP//RwDY/xYAAADx/yQA9v/0/xUA7f8fAOf/IAAxAP7/3/9MAAAAKQAdAEEADQDu/wcABQDm/+X/7P8FAAgACgD7/w8A8/98/wUA7f83ANv/GwD9/83/6/8vAKj/gv/1/xsA0f9AAAkACgCV//j/vP8GAAUAEQAhAG//JwAEAA0A7f8xAO3/EABFAB4ACAACAL7/uf9V/87//f8EAPj/EAAiAC4AAAAkAAAAGAAaAOz/FgAGANj/CQD//wEAAAAbAA0AJQAQAAAAKAA9ACoAOwAnAMT/GADe/xwASwAYAB8ACQAfABgAJAA5AAwAXP8YACEA6//c/7v/0f8nAB0Aff/9/6AABQAuAAoADQBNABYA/v8UAKn/GAD+/zgAJgAwAPX/IwDx/1QAhf8HAAIA3v/0//v/uv/9/yIACADIANf/hf8XAAcAyP8jAAkAAwDG/9n/Z//f/0X/3/8iABoADwC8/xUAFgA6AA8Avf/v//b/DwAdAF4A2/8nAOf/7/9OAPj/HQDe/ycAHgDaAAoA+f8CAPL/zP8EABUA+f/4/9z/LwD9/yUA9P/y/xUA4P8GAPn/uf/l/wgAe//8//D/9/86ADgAIQACACkA6v8jABAA///o/xQA+P8hAKX/IAAMABQAEQAGAPD/iAAl//v/rP8UABYA4v8tAOkAk/8NAAsA6v8dAPD/IAARAPX/JwD4/wkAHADf/xsA8/8QADUABwDi/0YA7/8mACcAPwAFAO3/CQABAPr/6f/9//z/CQAEAAMAEgDz/4b/JwDx/zMA2f8lAPr/2f/k/xoAv/+B/woAFQDU/0cAAQAVAKr/5f/z/wwABAAUABYA4f8qAAkAAADs/ycACAAEAAUAHgAKAAEAuv/E/1P/9f8sABoA9f8VABUANQAGABkABAAlABgA6f8VAAUAy/8HAAcAAwD7/xgAb/8aANb/DgAyADAAKQBFACEAqf8UAN//DAA/ABMAKAARADMAFAAfADoABABi/x8AJQDy/83/xv/D/zEAGgB+/xcAtwAKACwAFgDs/0QADAAkAA8Ar/8SAPP/MgAlACQA6P8UAAAASACR/wkADADe//H/9P/M//z/HwAXANEA0f8QABQASwDO/x0AIQD+/77/7f9z/9//TP/p/yAAFQA2AKj/HQASADgABQC9//D/7P8fAB0AEgDd/yQA6P/r/1QA+v8SAMf/HwAwANoACwAYAAcA8//T/wMAGgD9/w0A7/8eAPr/EAD5/+//BwDp/wAA9P+v/wMACQB6/xcA8//z/yoAQQAhAAwALwDq/ycADwABAG//FgD4/w8Ax//8/xYAFQAFAAoA/v+FAC///v+j/xoAJwDS/y0A7gCS/wsAGAAAAAQA9/8OAPP/9f8fAPv/BgAYANv/IADg/xUALQAGAOD/PwDs/yIAHAA/AAgA6/8SAAkACwDx/wAACgASAAIABADz//P/hP8AAN//MgDT/xoA6f/d//f/KgC4/4b/EgAVANr/IgD6/yAAp//4/+7/FgANAAgAHgDq/zwADAADAOP/HwD5/xoA/v8gAAUABgC8/7v/Zf/9/xoACwD6/wEADAA2AAAAIADj/ycAGgDr/xIAEgDO/wwA8v8AAPj/GQATAB8A5//y/zsALwAeADoAIACg/woA6P8UACoABgAmAAsAMAAvABcAKwAnAFz/GAAdAPf/0f/E/7j/LgAbAIz/EQDEAP7/JwAAAO//NgAIAB8AHACi/x4A4v84ADkAIgDy/ygA7f9FAJD/FwAUANL/BgDu/7z/+/8kAAgA1QDQ//L/FQAaAMf/HAANAAIAx//d/8f/4f9L/+r/FwAiAFIAo/8aABQARwAJALr/8P/+/xIAGwAqANf/LADc//j/TAAQACoAzf8bAEsA3wACAOj/AAD5/83/AAATAAMADwAQACIA9v8PAN7/7/8NAOj/CwDj/6r/+P8HAHz/8f/+/wQAJgAsACkAAwApAOL/IwAMAAMA0P8PAPT/FQBp/ysADAAJAB4AAAD+/30AiP/l/57/JgA/AOj/OQDxAI3/EAAJAO//7v8CAA4ACADh/yEABAAdACcAzf///9v/BgApAA0A0P9GAPH/HQAhAEQAAQDh//z//f8PAPH/CQDy/xcACQD2//3/7/+B//b/3/8uANX/GQAUAOj/7/8EAKX/eP8fABYA5v9BAOH/IQCn/+r/yP8dAPj/BwAOAOL/SAABAAwA4v8cAAkAAAD4/xoA4f/6/7r/uv9k/xQAKQD2/wMAEwARAEkAAAACAOP/FAATAPX//P8BAMr/GgD3//f/9P8kANL/GwDn//7/RQAsAA4AQgAaALv/BQDc/y4ALgACACsAFQAsAAUAHwAeAAAAcv8SABIABwCu/7X/xv8uABsAiP8TALoABAAdAPv/DAA2AAcAOABWALH/CADk/zIALAAmAPf/CADm/00Aiv8hAP7/tf8WAPX/4f/2/1wA/v/QAM3/AwANABcAyP8WAD4ADADL/+T/zP/h/13/3v/v/yIAUgCS/wMA/f85AAcAs//W/wQABAAlABMA3v8wAO//4/9NAP//JADL/xkAGQDZAAEA6f8HAAcA0//s/yYABQAcAEUAFADv/woA9v/r/xAA/P/9/+b/nf/X/wwAef8PAAUAAABz/xsAHwAPACcA5f/5/xAAFwCZ/x8A3/8DAGD/EwARABkABwD8/+3/gwA5/woAqf8xADcA3f8eAO4AkP/7/xEA6f/1/wYAGADn/+//LwD//wsAEQDM/xMAzv8jACoAHADH/0oA/P8pABcAbgAAAPj/9v/i/w0AAQAEALn/DwAFAP//yv/7/4P/8P/j/zQA7P8kAPf/6//o/w4Aqf91/yMATwDd/ywA5P8gAJL/JQC1/xYAGAD5/x4A7f8NAAcABgDg/xMABQDj/wEABQDf/wUAgv+p/2H/4f8FAO//9v8BABMAZwD///j/CAAfAA8A9v/z//b/4v/x/+b/9f8IAB0Awf8WANH/BABCACUACwBPACIAzP/v/9b/IgA9AA0AHQASABQABAAPAA4AGwBg/wwAFwDq/8b/zf/J/yIARQCN/wwArwAMABEAAADR/zAACAArACAAtv8cAOj/MgAlADMA8P8aAOT/SgCS/xgAIADK/x8AAADf//H/IQAAAM0Aw//e/xAALQDH/xwACgAAAK7/1v++/+//Y//v/5D/HwBKAM3/IgAEACEA6v+o//L/+f8QABQAIwDQ/zAA4P8BADcA7f8oAMr/HgAfANoAAADa/+b/vP/O/9z/EADq/xsA//8jAA0ADQDO//P/FgDn//P/8//L/9j/3/8aAIv/KgATAOX/HAC6/2sAXP8FAAoArv/G/9T/3P87/9H/7P8EAKn/0P/k/5T/UQDt/wgAHgD+//n/IwCw/+z/0//o/0EAI/8GAEj/EgAuANn//P+0/wQAov/v/zoA3/82AA0AKADP//L/7f8zACoAwf9U/yoAjv8zAPf/r//E/73/zP+f/wIA5P8LADkAFABHAEIAGwDi/wsAw//I//v/fv8NAOX/xf8MAJ7/AAAOABUAJgANAMr/4P/w/7f/BAAKANX/PAAFAOD/EgBp//X/4P/X//3/2v8KABYA/P8nAAkAsP8WAPn/FwDp/27/BgDo/w8Azv/6/8P/zP8XAAQAY/+s/83/4//o//b/2P8SAB8A7/8jAI//zv/p/9b/2f/5/17/EwApAAAANgDY/9n/GAAeAAIA5v+v/xcACQD+/8r/DgAAABUALQAEADD/7/9AAML/IAAGAPr/fP+3/8b/6v/x//D/9/8BAJf/AwAUABIAuP/2/ysACQAJANv/7v8UAAQA8v85AN3/JwBn/5D/l/8PABEA6P8XABQACQAjAKH/8f/o/5X/Bf/p/+3/7P++/wMAsv/H/9X/2f8UABUADgDI/ywA5P8NADkAxP/y/yAA4//D/w4A///g/xEA7/9dAO//VwC0/wYAxf/Z/yEAIQAjADIA8v8oAMn/wP8aABkAFQAlAO7/vf8bAP3/KQD//8b/zf/0/+r/4/+m/xsA5P/4/wcA3v9j//v/NAAFAPP/IQAEAAoABwD5/6P/xf/q////NgC2/wUAU/8XACgAxP/n/7D//v9f////EgCw/wYA9v8UAPb/zf/O/+H/CQD9/5//AgCo/xUADADg/4L/FwDs/x0A/f+x/xMAAAATAK//XQAEANv/AADR/wcA/P/9/wwA0P/f/xkAuP8IAOf/OgACAMX/9/+X/wcA2P/d/wcA3/8AABwA/P+x/9L/EwAjACQA3//y/xkAwf8AADAAMAD5/wcAvv8z/4f/IAAAABAA9P/e/wcA2/8AAB4AAAAAAOf/6f/i/9r/4P/V//L/KAAJACwAb//B/wYABQDq/xEAXv8/ANf/kv81AO7/8v8YABAAAQDW/+H/AADR/+3/s//9/97/AQAsAAgAOP8NABcA2P8VABEABgDn//v/+f/+/+7/vv/i/xwAxv/j//P/BgA6ACQAQgAHABgABwC6/xQA4P8IACAA6P/6/8j/jP/I/xUAKQDw//D/FADz/xIA9f/v/+D/lv+e//7/9//y//n/GgAGAPb/pv/0/9r/IgASALL/BQDc/xEAZP/h/xAAAAAOANv/DgDl/wsABwAAAOb/CQAhALr///9F/wEAIwAFADYADwARABUAsv/A/yYADwAXABwA9f/d//j/3P8zAPb/xf/n/+v/3//e/73/yP/j//f/FACD/7H/AgDM//r/AAAcAP//FgD4/x8Ap//1/9//7/8gANz/CABE//v/IAD3/wAAqf/c/5L/9P8OAPH//P8BABIADgDP/+v/CgAdAAsAx/8aALv/FgAVAAcAvv8cANL/IgC2//z/IgD9/yQABAA6AAEAx/8AAMP/GgAJABoAGADF//n/CADg/xMABgAxAP7/6v/q/7z/GwDZ/wMAAwAFABcAGgAdANz/hv/0/xgAEwD8//n/DQDb////5f8ZAAYAAAAEAPj/e/9LAPr/FwAaAOT/AQD+/+T/FgD4/wEADgAAAPP/3v/p//T/+/8UABoAEQCp/+z/AAD+/+7///+H/yQAsf+6/z8AAAD5/xQA+P8BANz/1f/5/ykAAgBs/wQA9v/3/yYADwA2/wYAEwD0/xAAGAAXALX/FwAGAA0AzP/Q/9X/DwCI//H/+f8FABgAIgAhAAoAEgD7/8n/LADz/wkAJADX/w0Amf92/+j/BQAPAPr/EQAqAAwAIwDt/w4AFACM/8D/DgAVAP3/0/8IAC0A7P9x//L/IAAyAAMA8/8cANX/GwDb/wMAAAAPAPn/3v8UAN7/CAANAOD/3f/v/1IAu//3/53/6f////7/OADY/+T/GQDl////JQALAPv/JQDe/8L/BADp/63/9v/v/87/+//2/9r/6P8JAMb/AgAQANj/wP/f/yAAEQD+/yUA+/8QAA8ABwDi/woA4v/r/+r/9f/s/1//6/8ZAAgA2f/O//v/8P/l/9z/DQAGAK3/EwAWAGn/7//Z/yAAAQDZ/x8Azv/7/+X/EgD2/5f/pf8sANz/BQAVAP3/HwB5/y4AEACx/wAA1v8bAPv/CQATAND/cv8WANX/EADS/ykABwAcAM7/3P8bAPP/xf8CAOT/xf8XAAMAg//+//b/AgD//8L/zP8EAGL/AAALAB8ADgAXAOX/8v9R/z4A6v8OAAYA6P8SAP//4/8LAP3/DgAAAPz/DQDu/9P/3v8MAAAAEgAsAAgA6//6///////9/17/+/+N/8H/OwD9/wAADAAgAAAA1v/X//b/EgDw/y7/FADw//n/HwAOAHz//v8JAOf/AAADABgAyf8oABYA3//q/9n/y/8EAKX/KAAAAAoAFQBAABIABQAMAAQAzv8kAPf/CQAYAPz/yP/a/4j/7/8sACMA4P/6/x8AAwATAPv/FgDm/0r/2/////j/8f/V/xIA/v8dAOD/AAD9/y0ABQDR/woA4P/3/w8A3P/f/xgA8//S/yIAl/8BAAsAAAAJAM7/HQDb/xEA0f/I/w4A4v9EAOr/7v/+/+D/LAAiAAkAFwATAAEAt/8dAO3/Yv/s//P////x/8D//v/U/+b/9P/8/xAAEADl/8v/zf8gAAAAJwDP/w4ADAD7/+7/DwDl/+L/0P/b/+T/TP/7/xgA+v/w/+D/CQCt/wIAx//2/+7/rf8YABAA7v/6/6v/GQDm/+H/FgAdAOD/5f+R////pP+d/zEA8P/5/w0A7/8dAMj/GwADAOH//v+v/xgA1f/7/xAA1//x/wAAzv8QAAcALAAAAAAA6//m//b/5f/y/wMA5f+7/yMA/f9u/9b/7P8LAPT/7P/U/wcA4P/+/6P/GQAOABEA8f/c/4D/LAAcABAACQDs/xIA+v/n/xUABAACACQA4P8KAPD/6P/a/wwAav8RAP7/EgDR/wYAAwALABIAWP/9/5n/xf9HAAoA5f8gAPD/BABG/9X//P8iAP//PP8FANv///8XABsAvf/5/+L/+P/z/woABQC8/xMA//8qAPL/CwDa/+b/m/8iAPP/EQARABEABwACAAkA0f+Q/y0AFQAGABwA7P/Y/83/2/9G/wYADQD+/wMADQAZABcA+f8uAAsAjP8HAAgAHAD2/9r/FQAMABQA4v8AAAAAIQAJAAUAHwDy/+r/JgDB/+r/DQDq/5L/IwCW//T/AAD4//H/2f8FAOL/9f/8/+7/DADy/0IAz/8GAAQA6/9DADcAEADx/w0A9v+Z/z8A9v96//D/BgDO//X/LgAAAAIA2f+2/wIAFAAvAMr/9v8TACAABQAgAB8ADAAIAOT/xf8MAPL/0//1/+b/5/+V//f/EgDK/wsAmP8VAP7/GgDR/wsAsf/A/woABQDZ/wQA7f8dAAAA4/8CAAoACACv/9X/9P/S/93//v/X/9r/+f/1/xkA2v8PAPr/xP/5/8H/GgCg/8f/CwDb//j/+v/V/xgA8f83AAQAEwDx/wMA4/+8/wMABgDi//v/1/8eAFf/tf/f//z/8//1/9j/CADt//X/1v/O/wYAGQAIAOD/Yv8VABcACgAVAPD/6P/b/wQADwD//9//wv/+/yUA9//J/97/AwD7//7/2v8kAPX/BQAHABEABwB3/+r/9P/r/0QA+P/Y/yMA0P8HANj/3P8PAAQA+P9C/xIAAQD8/yIAGAAAAPv/9P/G/+f/CwD4/7//EADo/zEAyv8wAOf/1/8aACEA7P8PAMj/9/8RAPH/DQDJ/2T/FwAFAAwAFQDo/9j/5//z/8v/FQD//wAACAD8/wQAEwAbACUA4v+c/wAAAgAnAPz/kv8sABMAFgD2/wQAyv8WAOn/3v8HAP3/9v8gAPH/4f8IAO//vf8dAMj/5P/q/xgA8f/W/xQA3P8GAPn/sP8WAP//RQBY//7/BwDh/zoAZAAXABQABgAXAMT/QQD4/7T/zP8PAOX/1P8HAOj/7//9/9///v8BADQASP/x/xgANgD5/yAABwAQAAMAo//w/+//+f/i/9z/EQDg/93/+v8jANH/CgCl/wgABgAkAD7/6P8RAJv/FwAAALj/6v/V//r/BgDl/xgAJAANANz/v//o/97/1/8kAAAA8f/C/9v/IwDp/yQAAwDL/wEAvv8SAL7/if/6/9b/gf8QAKb/BwDE/zUAAAAUAOf/GAC+/+n/8f8OAMv/8v8CAA0Ai//Q/8D/BgDz/w0A2v8GADMA7P///nb/7v8TAO//9P9Y//j/AAAYAAgA9v8rAPX//v8HAB4AAwCE//v/9f/z/7b/y/+I/5j/6v+0/x0A6/8FAP7/8f8bAJX/8f8lAPD/KgAPAND/IgDe/wkAy/+t/wkA6P8YACv/BQA4AAIAEAAQALD/8v/6/xgAz//2//L/y//u/+L/TQDj/xMA3P8IAOr/rP9C/xsAE//J/wIAHgAVAJD/NwDj/xIAEQAgAOT/zP85AP7/2//0//3/+P8CAAwADQAkAM3/FgAEAKH///8JABMAAAB3/yoAFAAWAPX/4f8AAA8A5P/n/9n/DwAlAEEAFADw/9P/7f/J/xQA/v8HAPP/AQDe/9n/5v+k/xgAvf/e//v/EwBlABMAv//y//H/HAALAB4AFwDq/xkAKQA2ABb/Uv+G/wMADQANACIA1v/r/6//zv/h/wkALADx/wQAJAAwAP7//v/J/w8Apv8R/w0Atv/z//T/KAD7/+H/3v/2/z8ABwDI/4v/BAD8/1oA0v8/ACYAof8HAAIADADS/5//MgANAMz/EwAKAMv/AQAHAI7/OwDk/0sAFQDh/97/xf8zACv/AQDz//P/BADp/xwAl//5/xEA2v8AABAAnf8GAOX/EgAIAPD/4P/6/ycAt//H/woAGAD0/ykAAQBt//z/7P/Z/wsAHgC//wsALADt/wwAgP8AAOr/FQAFAFb/IAAaAPD/AQDz//b/DAAbAPb/KwDJ/zT/7P/i/+L/ov/V/z0Az//K/18AIwDn/xcACwAyAC0Aif/y//v/AAAPACYA8v8dAOf/DQDT//v/EQDz/xAArf/f/+z/AgAgAA4A5/+7/+3/Xf/K/wcA9f/l/y8Ayf8VAPX/5P/v/zUAIQAJAHz/HQD//87/S/8JABMA/P73/wEANQD1/yUA6P/Q/zIA8f/A/9T/9P8HAB8A8f8XACYACwD///X/pP/z/83/OAAJAKf/CABxABEA0/8EANv/BgDb/+f/6P8IAB0AEwA1AP3/6f/j/73/HAAJAA4A8v/k/8L/4f9fAHX/AgDf/wkAEQAPAEcAN//g/+f/6//O/w4AHwDw/yEADQDC/zQADgBUAPT/+v/z/+f/9P/f/5n/SADp/w8AGwC//xL/9v8AABcA/P8hACwAFQASAEwAhf/s//b/8P80AMn/4/9p/wgANgCw//T/nP8vAHX/9f8bAKf/GQD//xkA4v8BAKL/4P8NAOL/zv8vANb/PgAnAI//pv8KANL/EQDL//H/DgD3//n/DgBdAAIAsP/h/9v/EQAGAAQAGQD0//D/GADk/xQA5v8YAPn/CQDe/5H/CwDX/9j/DQDk/zkAFwDi//r/1P8UABEAEwC8/+3/9v/Z//v/FwBKAOz//v+Y/x0AQv8MAAMA7P/3/wEACQDn/+r/MAD4/wcABAACAOX/6f/x/wIADQAVAPf/RACv/9v/FgAHANL/8P9T/xoA/f+n/zUA9P/h/x0ALgAJAPz/yf/9/woAEgC5/wYA7v8BAD8ADQAE/wMAMQDz/yEAHgAYABAA7//y/9v/5f85/+H/DgDN/wAA/v///xkAPwAsAA4ABgDt/+v/HQACAP7/IgDn//z/if+I//P/+v9GAOn/EQAsAAUAIADc/97/uP9e/6j///8XAAAA/v8NAAEA/P/d/wkADwAhABgA4f8LAN//FACX/77/AwARAAYAsv/y/+L/BgAMAO7/HwDD/0QAr//4/+z/8/8jAMf/MADI/wYAHwDK/7z/HgAUAEIAIgAHALb/GAAEAD0A5f/H/+z/BAD1/9//yv8OAOj/AwAUAOv/zv/p/9L/3f8FAB0AIwAWABMANwDB/+f/8//3/y8A0P///3z/GQAuAO7/8f/R//v/4//6/xkAmv/0/wMAHgDn/6n/4P9r/xkA+v/c/xAAtP8pAAwAsv+q//X/FQARANf//P8XABgAAwCn/2kA+/+Z/+j/0//M/yEAFgDs/7n/7f8VANr/CADp/ygA/P/0//v/of8XAM7/4f/5/9T/BQAOAPH/0P/C/wMAJgArANz/5/8QANb/DwAtAFgA8v/8/+T/AgCD/0MA/f8aABIA+P8GAPr/9P8fAPj/DAAEAA8A7//x/9X/6v/s/yAAEAAWAO7/5/8WAAoA+f8JAEb/MQCF/+7/WAD8//j/CQAcAAEA5P/z/xcAIwD2/8f/CwDb//f/MQD//1f/BQD4/wEAJAAvABYA4f8EABYA5v/e/8r/3/82AKH/4/8JAPj/MAAnAD4AGQAUAAAA4P8GAAkACwAaAPH/7v+w/3r/6v8PAB0A7/8sAC8A+P8WAN3/9P8DAGf/v/8AAAcAAAADABIA+P8CAOb/+P8HACsAGADg/wYA1f8dANv/CQACACIACgDh/wcA2P8dABUA6P/J/+z/LQDi/w8Aq//u//r/CQAyAM7/DwD5/9//xP8oAAgAEQAfAPr/xP8OAOv/KQDl/8X/6f/2/+b/4v+8/wUA5f8BAB0As//N/+T/1P/u/wsAHAAVABMADwARAIT/+v/n/+//AAD2//r/o/8LACEA8P/p/8P/7f/o//r/4P///+z/CwATAAIAs//2/7j/IwAJANr/IwDA/w0AFgDr/8L/5f///ycA5v8IAB8A+P8LAKX/SQD8/7H/4//L/wwAFgAEAAYAzv/m/xYA3P8XAPn/HAD///7/6//D/xQAwv/T//f/9f8AABMA5f+r/6z/9v8WACsAw//V/xcA0/8GAAQALwAEAPb/4f/k/4f/QQAHABcAEgDz//z/BADi/xwAAQAxABMAHgDn/+r/zv/7//z/EAASAAgA+f/z/xAABQD8//z/YP8rAFP/1/9LAPr/+f/6/wwAAADp/+7/DwAQAAAAs/8PANv/7v8nAAwASv8IABwA5v8dAC4AKgDS/xgAEQDz/97/4f/T/+P/vv99//3/AAAnAEQAGgAYABIA///N/xUA/v8LAA0A6v/b/8b/gP/l/yAAJQDn//L/JwADABUA1P8AAAgAl//c/wQABAD6/+f/BQARAAUA4v8HAP7/KwAVAPv/BQDX/w4A6//x/+3/JgD//8j/DQDn//z/EgD4/+z/9f8dALj/AACI/+n/+/+y/zcA1/8PAAIAxP/h/x0ABQAuACAA9P+s/xgAAQDC//f/1//x/+n/6f/j/9T/DQDk/wAAHQDp/+b/1f/q/w0AEwAeAAkAEAATAAMAtf/5/+7/3f8PAO3/7/8o/xQA//8EANv/3v/g/+v//v/g//X/5f/G/xgAEgCD/wsA4f8iABUA6P8ZANf/EwAFAOv/+/+P/9z/GgDY/yoAHwDk/w8Awf8fAAQAkv/t/57/EgD9//z/AwDU////DQDW/yMADQArAPr/BgD0/+f/EgDp/+v//v8GAAsAJwDn/7f/u//2/wIACgC4//H/DAB5/wcADQAkABYAAQDX//X/kf89AAkAEAAVAPv/5/8HAAMAEAALABwADwAdAPf/4v/g/+7/KgAIABQAHQARAPn/EAD///j/AwBd/wUAaf/D/04AAADt/xQABQAKAPj/3f/x/xIAAwDM//7/3P/w/yAAFwCl//3/FgD2/w4AJAAzAL3/IQAWABAA9/8OANn/DwDZ/wAA+v8GAB4AHAAiAA0ACQDw/8v/GQD9/w0ACgD3/93/av9u/9r/LwAXAPv/DQAhABcAFwDz/xAAFwA0//T////1//b/6v8KAAwAFgDI/wAAEwApABEA+P/0/+j/BgAaAOT/9/8QAPz/w/8XAOD/6/8DAAEABADa/xUAv//+/9r/2f/3/wYAPgDl/+L/BQCv/ykALAALAO//IAD6/3r/CwD7/3T/3//n/+//9f/6//H//f/p//P//f8dANb/1P/Z//n/IAALACMA4/8OABkA9//R/wAA9v/f/+P/9P/f/47/DAAXAPL/5P8CAAYA3f8FAL3/7f/A/8L/HAALALn/DgDK/ysADADg/wgA/P/o////zP/o/6T/i/8mAPD/AAAVAOP/EgD2/xYABADB//D/8v8ZAMT/8f8NAM//DgAYAND/JgDe/ycA+f8CAPL//f/+/9n/7P8GAP7/7v8dAOX/o/+q//f/DgD1/9j/1P8NABAA//+c/w4AJgAHAN7/5f88/yAADgAKABMA+f8EAAAAAAASABYAHwAAABIABwAIAJj//P8eAOr/EwAaACAA6/8WAAAABAANAE3//f9J/67/PQAKAP//GQDg/wUA+v/X//j/EgD+/7v/EADA//j/JgAeALT/AQD3/xAA9v8EAB4AyP8MAPr/OQD1/x4A4P+b/+j/GgDx/woA2f8WABgADQAAAP3/lf8mAPX/EgAaAN7/6f/K/8z/s/8OABMA+/8CABgAFAAPAP//GwBM/0L/9f8OAP3//v/S/xwAAQAaAPj/BQAAACEAAADo/wIA8//3/w8A5v/r/wkA9//Z/yIA4f/7//n/EAD5/+f/BwDc/wUA2v/4//b/+v9BANf//P8MAND/MgAmAAoAAwAIAP3/8/4OAPb/jv/9/xoA7//u/yoABwAFAOj/8v8JABgAHwDc//j/+v84AAcAIwD7/w8ACgDR/9T/+v/6/8//5f/b//7/Hv/q//r/6f8sANP/+P8HAB0A3/8PALn/1v8YAAYAvP8OAOb/IQD+/+j/DAANAOv/yv/P//H/uf+y/w8A+f/b/yAA/P8SAN//CgAJAPT/8//H/yAAwf/7//7/0/8CABAA0P8eAOT/JQABACgA7P8cANr/2//q/wYA9P/e/wEA/P8W/8n/9v8NAOj/qf8AAAoAEgD//9L/sv8DABgA/v/6/yv/AAAPABgAHAAAAAcA/f8AAAwAFwDO/5j/+P8NAOz/wP/k/wUA3P8AAAIAJwAQABsABQD8/w0A8v/r/8r/FQAWAA4A+f8lANz/CwD8/87/CQARAAIAy//7/7b/+/8nABcAuv/+/+//EQDj/w4ADwC4/9f/DABXAO3/DADi//j/9/8aAMX/FADZ/+3/DAD4/xAA2v9n/xsACwAaACkA6P/k/5f/6f+U/wwA+f/7//j/CAAZABQACAAgAJr/RP/z/wcAEAARALD/HgATADAA1f/w/+L/FADl/9D/AwACAOX/SQAJAO7/7//9/9r/HwDw/wEA8v8MAPr/yv/8/9P/MgDj/+z/BwDo/1cA3/8cABAA0f8lAF4AEADz/xcABADO/xwAAQDd////DwD+/+r/KQD4/wIADgDE/w8AFQAiAA8A2P8PAEQABgAeAAAAFgAKAMj/zf8XAAUA2P/O/+T/FwDi////IgDf/w0AqP8dAA8AGgDc//3/x/9C/xcA9f/r/wYA7P8aAAsA6/8DAB4A9/+q/+H/+f+h/8b/KgA0APD/5P/m/wwAzv8LAAkA4v/6/+v/CQCd//r/CQDO/9n/CQDd/xMA5/8rAPr/AwDo/x8AFADa/+3/BADZ//L////s/53/0v/1//7/6f8PAAsAEgAxAPX/b/+H/+H/IgDx/wIARP/z/xQAGAAWAPj/CAAHAAIAIAAdAOP/Z//+/+L/9P+D/+f/AADf//D/GwA3AOH/GAALAPP/FwDz/+f/IQADABgAV//r/zQA5v8DAOT/1f/4/xAACwDQ//f/DAACACMADwC///X/AAAyAMr/FAABAIz/DwDi/0IA6////+X/BgDc/+r/Kv8ZAMn/6//C/wIAEACz/+//BQANAA4AHgDv/9z/DgD1/8X/FgADAAUA7v8CAAQAFAD0/wYALf9G//3/BQBOABUAvf8dAAAAKgD4/wgA+/8aAPj/6P/x/xIAGQBHABMA8//7//X/6v8XAK3/BADq/wEA/f/i/wkA8f8fAOH/6v8JAN7/ZQDq/woA///P/xMAGQAZABgAAAAHAAUAAAACANX/gf8dAOv/8P8KAAAAGgDT/8D/9f8ZADUA0v/7/x8ANQADAAoA8/8VAAEA/P8VAP7/BgDQ//T/4P8BALb/3f8hAOH/3v+O/xIA8f8pANL/+/+P/+f/EAD7//r/3v/C/xIAGgDq/wUAEgD1/+b/5v/6/woA7P8uABsA2f8eANz/IQCn/w0ADQAMAAAAwP8ZAOP/+f/+/+P/if/q/+r/FgD7/yIABwACAPj/AAAcANf/wP8SAA4A8/8QABwAf//8/+j/8v8AAPv//f8QACEA6f/b/1f/+f8GAAYAFwCT//P/OAD6//3//f8TAEsA/f8ZABoAuf+H/wgADwDy/8b/3f8BANf/5P80ACYA8v8pAAAACgAVAOX/+v8WAAcAGQA0AO3/HgDJ/xIA7f/d/+j/AQAOAM//9f8NAAoANgAbAOb/+P/4/8X/mf8lAOL/y/8AANz/GQDs//L/9f/8/wIAxP9h/yAA9P+8/77/+/8GAN//4P8DAPT/BAAXAOr/5P/+/woA8/8KAPz/FAD5/wYAFwAiAPT/+f81/43/6P/n/yMAFwDI/wsAawAwALH/EQAEACAA2f/L/+P/DwA1AF8ALQD+/+P/9P/G/x8Az/8LAPX/AgADALz/LwABABMA6v8WAAkAw/9jAL//BwDp/8b/9/8TABMANAANAAQAwf8zAAoAQADc/wYA/v/s/8v/+v/C/yAAyf8TACEA0f8OAPf/5P/o/xIALAAgABgAEwAvANf/9v/x/wIACgDv/+r/lf8FAA0A+v/w/9v/HQC4/wEARQD+//L/HwAQAPb/tv/6/9f/EADg/+v/7P/u/zkADwCg/7//6f/r/x0Avv/j/ygABADt/wMATwAGALT/6P/B//r/KwAbAP7/AQD9/xoA8f8ZAPX/FQD6/xkA+/+h/x8AlP/3/wcA5v/0/wMAEADk/w4A9v8cABAA8//p//L/2v8RAP7/SgDs//X/r/8RAID/HwAcAPP/9v/9//f//P/h/ygA6P8QAAoAGQDm//D/0v/6/+L/CAD2/wAAv//l/xYAEQAKAAgAV/8fABQAwf9HAPX/7P/0/xoACQD0/9P/7/8XAAgA8v8RAOf///80AAsAiv8LABAA6f8mAAkAAAAFAOH/BQDl/+b/x//p/yoAVf/f/wkA5v8sAC4ALwAGAAUA7f/v/woAxf8JABUA9P/n/7f/Vf+6/xIAIwDm/x0AIQANABcA4f8MAEf/Z//L/wsACwD9/xcAFwAHAPX/8f8CANj/FwAqANb/FwDj/xoA2f/i//3/CAACAOP/wv/Y/wsAEAADAOD/6P8mALf/+f8EAPD/GwDv/xcAuv8MAPv/uf/N/xUADAAsABIABgCv/zYA//8+ANn/6f/+/9//3//d/87/EgDq/xEAGwDi//b/8v/+//H/CwAkABkAEQAPAC4AoP/s//j/6P8YAPb/AQCX/xsAMADk//D/3/8XAPX/AwA5ABEA8f80ABQAEgCB/wUAyf8cAPj/6/8fAB4AGwAnALf/uP/e//f/JADc/wsAIwACAOv/of9hAPn/sv/h/8f/rf8iAAMAAgDs//v/JADu/xsADAAJAAMAFQD+/8D/JQDa/9f//v/x/xUACgDr/7//6v/+/yQAJgDs/9P/DgDu/wsAEQBAAO//CADy/wYAOP8eAAQAHwAlAPH/DQAEAOn/JwD6/xIADwAXANL/5v/c//L/+f8nAA4ABgDv//7/GQAIAO7///9H/xoAzf/X/14ABwD1/wIA9v8FAO3/zP8QABoA+f/V/yIA3//1/x8ABwB1/wMAyv/o/y0ALAAkADAA2////+j/4//X/9z/JwCn/9j/AADj/xMARgAzABQAEQD+//P/AAD7/xIAGQDo/+X/if9//9z/KwAiANr/EAAnAP//DgDx/wkAS/+X/+D/CAAVAAIAAAAcAP7/6f/s/wgA6/8YAA4A8P8HANP/BgC1//r/7v8bAA8A3P/f/+v/CwAQAAAA0f8HACsAq/8CAIX////4/+n/HgD5/w0A+//S/9z/HgACADQACgD2/8P/GADi/yMA+v/l/wAA3P/h/+j/1P8PANn/BQAfANz////e/+j/AwAOABkAEAAKABQAFADG/+b/8P/h/9r//v/X/6z/GQAKAOz/7v/o//7/4P///xEACwADAB0AGAD9/+b/AQDL/xkAAgDz/xEABwAHADwABgDn/+n/JwAGANL/MAAkANz/5f/E/1UA7v/M/8f/uP/z/x8AAADz/+7///8VAPL/JQACAAsA9/8fAOP/1P8IANX/6P/8//z/DQAbANv/zP/O/wUADAA5ALH/5P8HAOf/EgD//zEA/v8CANj/+P86/xcADgAfAB8A9v/m/wsA6/8AAPH/PQAMAB4A4P/k/9z/AAAIACcAFwAVAAEACgAUAAkACgD6/3n/FgAX/8z/UQAEAPD/9P8AAAcA6v/2/xUAAAD+/8r/GQDk/+//GAAPAGv/AgAQAPf/KAAbADUA1v/u/yoA3P/o/+X/4P8NAJL/0/8BAOr/CAAhACYAEAAMAPP/4/8CAAUAFwATAOv/1v/R/3X/x/8yACAA3v8hABkAAAARAPf///+1/13/2/8FAPL/9f/z/wYABwD2/+3/DwAAAB4AFADe//3/2v8RAPD/BgD4/xEABwDW/wkA7v/9/xgADADn/wMAGgCI/wQA7//k/wAA9P8mAKH/8v/k/7f/1/8iAAAA/P8MAPL/z/8XAOn/0//0/9v/BQDh/+//2//Z/woA9f8DACMA2v/k/+f//P8DAAoAIADy/wIAEAD5/+L//v/1/+j/8f/s/+X/q/8WABoA+//t//P/+//k//L/9f8VAPb/0v8ZAAkAf/8KAMn/JQD2//X/FADe////JgDt//T/pv/4/xgAy/8AACIA7P/7/wIAFAD6/87/2P/V/xYAEgAFAAcA/P/5/w8A5/8pABwAEgD//xwA8//v/w4A5f/j/wUA/P8AABUA0P/K/63/+P9n/xgAwv/f/woA+f8GAPD/FgAQAAYA2f/8/3n/JgASAAcAGAD8/wQABwD1/w8ABwBGAAUADwD3/+T/3P/x/yMAFgAVADEAFAD6/xUABAABAPX/oP/6/1//zP9TAAoA9P8HABcACQDz/+r////+//v/0/8NAOn/9f8QABkAnf8CABQAAQAUAA4ANAC7/+P/FQD3//f/CgDa/w8AaP8YAP///P8eACwAJQAJAAEA7P/m/woAAgASABIA9P/r/9H/cP/n/xQABwDs/ycAFAAUAA0A9f/1/yr/dP/1/wAA9f/5/+H/CwAFAAgA/v8VABIAHgAOAOb/GwDn/wIA9P/p//7/GQACALn/FwDk//z/DwAIAP//BAADAKT/FQCy/+f/DwDf/zEA0f/C/xIAtf8dACEA+/8DAAwA7v+4//n/5P+p//b/8f8IAPD/HADp/wsA/f8EAPv/JADm//v/8//2/x8ACgAhAPD/AAAMAN3/2f8AAPn/xv/d//H/4/+2/w4A8//r//n/6v/t/+3/+P/a/w8Azv/U/xMABQC//xYAyP8lAAIA8v8GAPv/8f/3/9j/DgCf/+P/HwD6/x8AJgDz/wcAAAD2//P/3v/h/+D/KAD4/xAA/v/8//7/DgDi/y0ACgAXAPj/HADy/woAAwDW/9n/DQAHAAAAGwDf/7n/nP/6/wgADADC/wcACwD1/wEAnf8ZAOv/HAD+//b/Zv8PABwACgAPAPr/5f8RAAEADAAQAC0A/v8NAAcA/f8DANz/JQD4/xcAGAAiAAcAGQAIAAcABwDz/+T/z//m/zoADgD5/xUABgAOAP7/5f/7/wwAAQDd//b/6v/3/xgAHwC7//7///8HAPj/BAA2AJj/+f8fAC0A+v8MAOD/5//W/zAA+P8DAOH/BAAZAAIAAADw/9j/LgD9/xEADQDs/+b/fP/f/6T/AwAGAAAAFQAUACMADQDp/wIALf9M/+//CwAEAAMA0f8PAA4AEADp////CwAdAA8A7v8PAPT/6P8zANz/BgD//wkAvP8bAPj/+f/7/wwABQD6//n/xf8WAN//6/8YAAoARADd//b/IQC4/xkAIgADANv/CgDm/4//5f8IAKb/CgALAP7/+v8wAPr/CQD///f//P8iAPD/v/8AAP7/NAAJACcA8f8EAAcAzf8CAAQA/f/M/+r/4/8JAIz/DQAKAP7/1v/a/wAA7f8IANT/GADL/8n/FAD3/7r/EQDf/x4AFgDy/wcA8f8LANj/7P/7/8P/zf8rABEA9P8TAPf/DADy/wAA+//3/+D/7/8LANv/AwAHAPn/0f8AAOP/KwDr/x0A+/8IAPT/FQAHANf/y/8LAPj/+P8NAPL/bv+7/+7/FADt/87/IgAOACAA9v/g/9L/AgAUAPv/7v8s/wQADgASACMA+f/5/ycAFQANABkAAwCn//j/+//z/8L/5v8UANT/DAAGACcAJgAdABIAFgAHAAAA7v/p/9v/GAARAOv/MwC6/wkA9v/g/wEACAD5/+H/9/8TAPz/GgAdAKr//v/z/xgA3v8FAB0AiP/q/+7/VgDx/wAA1P/g//L/IQDa/wsA3v/w//r/7P8EAMX/gv8hAAAAEwAMANz/4/+y//z/y/8HAP//BAAKABIAEAAFAOn//P9a/1f/9P8PANH/FAC1/wsAHwAsAOX/AAD4/xoA9//g/xoABQAAADcA5v/h/wEAAAC//xoA7f/q/wAACAAAAAMA9v/k/ykA5v/z/x8A7v9sAPT/+/8WAOL/HAA+AA4A+v8TAP7/pP/z/xIAvv8BABQAAgDv/zMA8f8/ABMA7f8JAB0AHwDL//z/0/8yAAYAJwD2/w8ABQAAAAgAFAAKAL//7//g/+//ov/7/+X/+P8hAIT/BwDw/xEA+/8tAOT/uf8ZAOn/1v8RAOP/FQAOAPH//P8jAAsAy//K/+3/z//g/yEALgDR/xMA+P8CAOf/CQD5/xsA8/8EAAcA5f8NAO3/+f+r/+n/3/8fAOb/JgD1//7/AAAXAPv/+v/D/w4ADAD8/yEABABV/8P/+P8RAPT/6P8iAAoAJgDx/6n/r//5/xEABQD+/zn/+/8FABcAIgABAOr/KgAZACsAFwDO/6T/BgD5/+L/vv/j/wMAzv/s//P/KQBIACAAHQAOAAwAAwDz/yAA8v8GABwA8v9FAK3/CwD3/+r/+v/4/wMAzv/7/yUAAgAcABQA0P8DAPr/MgCl/xIAAwCI/wEA6v9nAOf/8//i/wYA7//g/1X/EgC0/6//xv/s/wgA1v/h/xAACQAPABEA5f/V/+b/DADb/w8ABgAQAAgACQAkAAwA3v8IAEj/U//2/wwADAANALX///8fABAA1f8KAP3/GADw/9f/BQAVAAQAUQAPAOb/9f8LAMH/GQDv//D/AAAGAA4A6v8LAO//HgDo//H/HADa/2sAAAAWAM3/uf8kABEAGgANAA8A+//b/+n/9v/d//P/BAABAPn/KQABACcAyv/i/wQAIwA6AAAACAD2/0EADQAkAPf/EwAAAA0ABwAXAAsA6v8AANn/9//P//j/LwD+//T/pf8JABv/JwDi/xsAzv/h/xwA6v/M//L/Vv8OAAwA2v/0////CgDY//j/8P+9/+f/BwAwAAUAAADx/xMAuP8eAAcAGADz//v/4v/Z/+v/CQAIAIH/Xv/1/x0A2f8mAAUA9v/y/wEACADq/9n/DAAVANv/DwAVAHf/uP/g/w0A+/8GAAUACwAHAOL/4P+s/9v/CQAVAAwAkP/w/yEABQAaAP3/BgANAAQACQAWAPL/YP8YAAUA+/8AAPb/EgDY/+L/9f8vAAAAIgAUABAAAgAEAPP/JQAbAAcALwDc/zoAvf8NAPT/7v/n/wgAAQD6/wsA0f8IACoAEgDK//7/+f8aAL3/DgASAG7/BADu/28A6v/y//L//f+D/7v/N/8XAPH/6P9L/8b/AADN/8H/FgA7AAwA+f/4/77/9P8WAL7/CQD1/xgALAAOABYAEQAjAAAARP+U//L/2/8kABMAkf8IAGIAMgDp/wsA9v8UAPL/wP8BABgAEgBPABIA8P/6//X/6f8SAN3/9v/6/xUA+f/y/wgABwAhAPP/9P8RAOr/KwDy/wsA/f/I//n/FAAHAB8AAAD3/9j/PgAhAB0A+/8JABcA4v/p/+3/yf8iANf/DgANAMj/FgDz/+z//v8OAB0ACQAQABUAFgCu//T/8//6/+//BADD/03/GwAuAPb/+f/x/xAA7f/3/zsA8f8aABAACwAVAOP/AQDw/xQA4v/q/woAEQAZAPf/v//T/9L/GADn/9b/9f8WAAIA4v/e/zQACwDp/9r/0/8FACgACQAJABoA8P8MAPr/DgAHAAYABwAlAPz/2v8ZAOD/1/8PAOD/FQALAP3/5/8mAAoAHgAFABIA5f/T/wMAFwAHACUA/v8GAHP/DgCK//7//f/z/xIACwD3/+7/9P8hAFv/GgACABUA1f8KAOX/+//u/xcA9P/k/+f/CAAQAAAABwAEAD7/EAABAMH/UgALANr/8f/p/wYA7//2/wcAGAABAP//BgDk/wgAFwAHAOb++/8aAPn/LgAxABQA5v/k//3/6//9/9L/9P8AADP/7P8FAMn/KQAlACAADgACAPL////9/+3/FgATAPj/3v9o/7z/+f8RAFYAzv8oACUA9v8KAOr/EABK/+v/2v8DAAAA8//8/yQA8v/m//P/+//I/wsAJgDt/wMA5P8KAM3/6f/4/x0AAQDs/9f/6v8TAAkACwD4/wwAMQDD/wMA7f/+/wMA8f/n/7b/CADq/8j/1/8TAAIAPQDy//X/uP8vAOf/JAD3/wAACwDK/9n/AADS/x0A5v8HAA8A3P/9//j/AADz/wwAGAAGAAwADgANAMv//v/5/+n/7f8IAOX/Tv8WAAoABQDy//P/EQDl//b/LAAHAPv/IQASAAgA+P/8/8r/EAD6//P/IQAvAAUAKwDN/+f/xv8JAAEAyf8IACkA+f/Z/9r/VgDz/+f/xv/a/9T/LwD8/+3/EADs/wsA+P8WABQA4v8AACsACQDY/wwA1P/7/wgA9f8PAA0A2P/0/xAADgAUAAoA3f/l//T/+/8WAPD/JAD9/xQAr/8JAC7/DQALAAgAGwD+/wQAAQDu/xwA6v8VAAoAGgDv/+7/6P/4/wMAJQAMAAAA7/8KAAkAAwACAAAAQv8XANH/wf9LAAgA6//r//z/CgDs//L/6f8gAAQA/P8NAOz/AQAcAAkAgv8FAAwA7f8rAB4AKwDp/87/CAD2/+v/2//l/woAX//q/wEAvP8QACkAJwATAAkA9//s//H/8/8TAAUA9f/N/1L/ev/s/xoAKQDO/ycAHgD6/wwADAAFACH/zf/t/wMACgD6//r/GgAIAP3/7f8EANr/DwARAOr/DwDc/xwA3f/p//z/GQADAOj/5//u/w4AAwAGAOj/FQAfAK//9//L/wAABwDw//H/0v/y/+r/w//E/xUA//8JAPL/2v+t/yQA8/8XAPP/+P8JANz/6P/m/+r/GQDn/xAAFADW////8//7//z/CgASAPv///8LAAcA2v/4//X/6//+/wMA3v+f/yEAKwADAPP///8AAPv/9P8MAAEADQAMABEAAgDX////zP8SAPr/+P8eAEkABQAjAPD/8P/e/yAABwDV/xEAHgDY/+X/0/83APL/z//h/9n/+/8WAAAACAAPAPP/CwAAABoAHQDv/wIAIAD1/+n/EwDI//H/BADq/wEADgDw/9H/+f8dAP7/LgDf/9//BQAEABEA7/8bAPb/BwDV/wcAKv8NAAwACAAFAAsA+v/6/+X/EgD3/zwAAgAUAOn/8v/l/wQAEwAeABYAMQABAAAAAAAFAP///f+K/w8Atf/N/0cADQDr/+//+v8GAO//6//y////AgD5/woA3/8AAAwACwBu//7/FwD5/ycAEgAuACcA2/8lAOv/6f/u/+T/1P+5/wIA/f/m/wEAIwAkAA8ABgDz/+3/+v/2/xgAAQD1/+T/0f97//z/IgAtAOX/JQAQAB4ACQACAPz/QP/g//P/BAAAAPv/9v8ZAAMA/f/w/wwA6P8RABsA8f8SANv/AQDg/xEA/f8ZAAAA2/8CAM7/+/8TABEA5P8jABoAqf8GAIP/6f8SANj/+f/R/+//CQDH//D/HgABACsA/f/p/67/EQDq/93/9P/u/woA6f/0/+z/7/8mAPz/CAAaAOb////s/7P/AwALAAwADADw/wkA+P/s//T/+f/p/+T/AADu/7f/GQD9//r/AwD7//b/8f/u/wYA9P/0/9z/DwD1/+n/CgDJ/xMA8v///wwADwAxACEA9v/7/83/9/8EAO7/FwAdAPT/8f8MABYA4P/T/9r/0/8AACEADAAAABIA//8YAP3/HwAdAPv/+v8iAPb/+v8RANX/7/8FAPH/+P8OANj/3//U//z/qv8RAL3/7v8PAAYAEQDf/xMAEwAPAPL/FAAk/xUAGAAEAA8AAADU/wMA+f8XAAIAPAD+/xsA8f/4/+j/8v83AAoAFAACABEAAQAIAAgAHQD7/9n/AgDX/9v/OwAPAPT/BADt/wwA8//r/+f/DgAHAAEAAADZ/wAABgAOAKz/AwAOAAkAFAD4/yoAx//R/w0AAADy/wkA4v/j/6L/FgD9//j/BAAoABgACgADAPD/9P8HAAIAEwAHAAAA2f/e/9L/z/8CAP////8bAAYAEwANAAQA9v82/6v/7f8AAPb//v/a/xAAAgAAAPv/AQAAABAAEQDw/xQA5/8KABEA+/8GAAsAAgDU/xIA7/8AAAoAEAAJABYA8v+z/wwA+P8CAAgA4/8CAMf/1v8bAMz/AwD7/wAA+P8DAMT/u//9//r/xf/+//X/EAD7/wsA9v/h/xcA+P8EABsA5f/2//L/BgAEAAsAFADy/+n/AgDm/wAAAwD+/+P/6v/5/wEAkv8KABMA///v/wEABAAFAOz/9f/4////8v8WAAAAyv8NAOz/FADj//r/+f8EAPf/+//3//P/0//k/xcA+v8DABsA9v8EABQAAgDn/9n/3P/p/wQACwAEAAYAFwALAA0A//8gAPr/AwABAAgA9P8FAA4Az//Z/w4A8v/v/w0A6f/X/7z/9P/6//7/uv8MABAACwADAMv/BgAKABgA/v8CAEP/DQATAAUAFwAEAPj/CAAPAAYAFAA5APT/NQD5/wAA9v/z/xUA6/8WACgAHgABAA4AFAAjAAEA8v/9/9P/5v8xABEA8/8NAAIACQDz//T/BAD1/wMAAgAAAO7/AAAKABEAu/8AAP7/EAD5/+r/HQDC/3b/CQAJAO7/FwDh/+v/4v8lAPP/AwDm//z/DgAAAAUA3P/2/xoA+f8OAPX/9f/i/xIA9v/O/wwA/f8EABMAEQArAAcAAgAAABL/mP/5/wgABQAIAMX/CwALAAQABgAHAPH/EQAXAO3/CQD+//L/BwD3/wgAAQAAALD/HQD7//b/CQAMAA0AEwDz/87/EADf/+L/EADv/xgAwf/j/yQA1f8WABQA/f8AAAsA5v+l/8//9P+6/wwACwAVAPb/JAACAAoACQAJAPj/GQD//woAFQDz/xgABgAYAPT/AAAHAOT/CwAKAAcA1v/c//f/EACi/wQA3P8AAA0Ay/8BAPT/5P/1/xUA6P8DABIA9f/x/wQA5P8LAO//7v/k/wUAEwDk/+T/4f/n/+r/GgAdAOv/EQD9/wwABADx/+X/BgDd/+v/AAAFABsA+P8WAO7/HwD6/x4A8P8GAP7/FQADAAsAEQDy/+P/EAABAOr/BgAIAMX/x//u/wIA8/+3/zQACwAcAPf/nv/v/woAFAAUAAsANv8KACIADwAQAAQA+/8ZAAAAFgAUAPX/6f8HAP3/9//a/+r/DADY/w0ACwAYACQAGAAaAB8AAwAHAAIA3v/0/xEAFgD1/x0At/8LAPD/8P8EAAAAAQAMAAIAAwADAA4AEQDh/wkA7v8SAOb/6v8DALb/6P/y/ygA6P/4/+L/AgDv/wEAy/8KANz/0P/1//r/DADt/+n/FAACABEA+v/v/97/1P///7D//P/9/woAHAAUABwACgDr//r/cf8q//f/CgD6/w0Arf8DACIAMQACAPr/+P8QAPv//P8IABEAFQA7AAgA6P/+////zf8eAPv/AAD9/wEAEQAJAOT/7P8VAAYA5P8QAAMASQDM//f/DADX/yEA+v8KANP/CAD1/7H/4P8RAM7/IQD6/xIAAgAeAPv/BADr//L/AwAZAAQACgD6//X/GQAKACAA9/8FAAAAAwAWABYADADp/wAA3v8UAJv/FAD9//v///+y/w0A+//h/+3/EQDr/8//DgDi/9z/CQDM/w4AAADk/zT/8f8ZAOf/4P/x//z/6/8ZAC0A9P/+/+v/CAD1//D/9v///+n/4P/6/wkADwADABwA4f8GAPn/FwDZ/xoA///3/+7/DQADAO3/7f8SAPv/8f/7/xoAU/+2//P/+P/z/+H/RQALAAgA5P/+/93/+/8OABAA+f8m/wQAIgAJAB4A/v///yIADwAZABUA9//D/wgAAAD5/+H/+f8VAMz/AAD0/yIAIwAYAB0AIgD//w4A9v8IANz/4/8bAPP/KQCZ/wQA8//1//T/+/8EAAEAAgD//wcAFAANAM//BADy/ywA2f8LAAAAwf/h/+b/NADu/+r/5v8CAOz/8P9O/xAAwv/P/8z/6/8KAOv/2v/8//P/CQDy/+X/z/+//wwAuv8BAAAACwAWABEAIAAEAO3//f+3/zL/+v8EAPn/CQCa//j/BAAwAPj/DQDz/xIABQD8//7/HAABAC8AFADz////AgC//x4A6f/t/wAACgAQAAUA7/8NABAAAwDa/xUA9v9CAMD/BwAKAOL/MwAEABUACAAKAPT/xf/l/xQA7v8cAAgAJAD3/yAAAAAlANL/8f8EABUAMQD5/xYA7P8PAAkAHAACABEAAgAJABAAEwALAOb/AADp/wIAxP8UAMP/7v8GAHT/AgDz/8//BgAMAN3/7f8SAOL/8//1/9D/AgACANv/5P8aAAwAzf/j//D/AgDt/xcAQgDk//f/9/8NANz//f/8/wwA6f/m/+b/+f8QAP7/IQDP/1D/+v8PANf/HwD9/w8A9f////X/AQDx/xMAGQDg/xYAGwAU/3P/6v8AAPT/6P8iAAUAAADc/9f/wP/s/wUACwANABv///8oAAEAGwACAAUANgALAC4ADgDY/73//P8CAPL/+f/v/wcAvv/m/+b/JAAsACAAGQALAAoAFwDq/xkABADd/x0A/P8kANX/CADy/+n/AAD9/wkAAgADANv/DQAWAAsA8P8HAPH/MgDV/xUA/f9+/9b/6/86AOv/0v/s/w0A4P/m/y3/FgDB/7r/0P/t/wYA2f/V/w0A5/8LAPn/DADS/8D/EQDP/w8A8/8XAB0AEAAiAA0ADAAJAHP/Qf/0/+T/EQAVAJL/BADZ/z8A7/8DAPD/CwD6//v/BAAjAAMAOAAXACcAAgAAAOr/GgDZ/wAAAAAXAAsA/v/9/w8A9v8JAOT/BAAEACsA1P8NAPP/m//o/w8ACAA9ANT///+j/ykAAQAjAP3/IQANAPP/6f/8/+v/JwD1/xAA/v+8/wAA9f/n/wwADQD2//X/EgALAAT/vv/r/wMABADv/wIAzP9V//v///8MAPD/9f8IAPf/8f9KAPz/HAAOAAYA/f/6//P/8/8LAO3/8P/+/wcAEwD2/9X/4//X/yQA6f+///H/EQD3/8D/2/8jAA8AHADl/9b/HAAnABIA9/8hAPT/7P/7/wgAHgDv/wIABgAKAO//DgDY/+T/DwDf/wEAGQAMAP//IQBAAPb/KADp/87/oP8DABsA//8XAP3/CgC2/wIAsP8GAP3/9f8KAPz/DQDw//j/HwDZ//f/8/8SAP//CgDx/wMA8/8WAPv/0v/t/xgA+v/r/w0A+P8z/x4AAgDk/xwAAgDy/+z/+f8QAPD//f/8/xQACgAQAAEA6v8QAAYA9f93/wsAEgDw/y8AGgAhANT/5/8GAOr/9v/L/+//GACO//b/BQDI/xcA5v8UABoACAD3//n/6v/9/w4A/v8QAPr/v//Z//L/AQD1/8n/FgAbAAEADQAQAP3/p//6/+b//v/z/+b/8/8ZAAEAAADz/wMA3P/+/x4A7P8RANj/CQDY/9b/+/8PAAAA8v/Y/+z/AQD4/+//8P8MAA8A3f/9/+X/NgAPAPT/sf/c/wAA9/+w/9H/BAD+/xgA1P/s/7v/MAAcAAkACAAJAA0A3f/g//L/5f8pAPr/EwADALf/BAD8//b//f8NAAEABgADAAoA4v/H/////f/9//z/EwDc/4r/FwAlAP//7v/+//n/AQD7/zAA//8YABkABgD9/xgA/f/W/wYAAAD2/wEAFwACAP7/6//h/+n/KQDM/9z/+v8SAPn/vf/d/ycABQANAN7/2//g/wwAAQAEABsA6f/1//v/CwAXAOj/BQAEAAAA9P8XANv/4v8KAPL/+v8SAAcAAAAnADQABwAZAPj/2/+y/wcAFwAAABEAAwD9/8j///86/wEA/P/9/w0ACwAPAAAA+f8iAOD/DAD1/xMA4/8IAOb////8/xcABgDw/+3/DwDX//j/BwDx/1f/FQDx/+D/JwAMAOz/5/8AAA4A8P8bAPP/AAAGABEA+P/z/w4ACQD6/3r/AgAPAPX/KQAfABoA3f/l/xEA6P/z/+f/8P/6/63/AAD9/8X/IQAPAB0AEQAJAPL/9f/s//n/EQD6//z/7P+u/9D/7f8TAP//1v8UAA0ADQAOAAsA+f+y//n/+f/9/+3/9v/v/xsA7P/1/+v/BgDj/wIADADj/xEA0P8CAMn/+f/5/xYA/v/2/9//1/8AAAAA///j/xwARwDI/wAA0f/7/wkA6//L/9P/7v8AAMj/1P8LAAUALwDc/93/kv8aAPj/CgAGAAgAEQDo/+T/AQDr/yYA9v8LAAUAy/8FAPz/BwACAAoA+//3//j/BgDR/+b/9v/+//X/6v8OAOr/rP8aAO3/EgD3/wcA7v/O//3/IAAAABcAFgAMAAAAFgADAND/BAD3/wEACgAOAB4AHQDq/wEA0/8NAOr/1v/8/xMA/f/L//j/IQDw//T/4//p/+3/BwAPAPz/GgD//wgA/v8QACoA7P8CABkABgAHAA8A4//t/wkA7//7/xQA/v/9/wIAKADu/woA7v/e/+v/CgAXAPD/GAD4/wEA6P8iAHX/+v8IAPf/AwAFAOr/AAD1/x4A8/8YAPz/BADf//P/7f///wYAFAAPAAkA9/8OAL3/9/8JAPL/n/8GAOf/5P8aAAsA8P/m/wUAEAD0//P/AAAYAAUADwAJAOj/DQD5//b/mv8JAAwA+f8gABIAGwDR/+z/DADt//H/8//q/w0A2P8OAPv/6f8PACoAGAASAAIA8P/3/+v/+/8WAPT/AwDO/+X/zf/k/wIA9f/t//P/AgAcAA8AGgD+/9X/8P/2//3//v/5/+T/GwD9//7///8CAPf/AwAHAO//BwDX/xQA8v/0//b/AAD+/+7/8//t/wQABwAJAPb/HwAJAML/9v/1/wUADADx/+X/3v/d/wIA1//l/wAABgAMAOj/SQCa/w0ADADv////AQANAPT/6//0/+b/EgAOABAABgDI/wEAAAAAAAAAAwAEAAYAyP8DAOP/+//0/wAA9f/0/xAA8P/E/wAAGgD+/+//BADq//X/9f8XAP7/FAAAAA8A8/8XAAoA3/8GAO7/AgAKABMACwAOAOT/AQDx/wkA7v/p/+3/DQD6/+n/CQAMAO3/5v/s/9n/+v/4/wQABQAfAPz///8DABEACADz/wUAAQDz/wQADwCt/+X/CwDf/+D/FgAAAPL/9P8AAMn/AgDa/+b/AwANABEA4v8MAO//DQDv/woAOP8AAAIA+f8BAAQA7v/4//X/DQAIACUA+f8fAJ7/AADv//3/DgAJABIADAACAPb/3f8FAB0A9//W/wcA4f/a/xgADAAAAOj/AQALAPL/BQDl/wEAAwAZAPH/4/8LAPD/9v/H/wEAAAACABIA+P8DACIA8P8IAOz/6f8LAOT/8f/r/ykA6v/7/wMAEAASAAgAAADv//L/8//t/xYA8v/8/9f/7//d//X/+v/z//z/DQACACcACAAAAP3/qv/u//f/AAD//wAA2v8YAPz/+f8YAA0A8////wEA8//2/+7/CQDj//v/AAAGAPv/5f8EAPj/AQASAAcAAAAjAOr/0P8AAOX/9P8RAOj/5f/U/x0ALgDn/wUA+f8DADAAAADh/5z/4/8BAAAA8P/4/xQA9v/+//z//v/8/w4A+f8KAOr/AwD+/xYABQAFAAUACADO/wEA2f8DAAUADADz//f/CQAYALH/DwDZ//v/9v8FAAAA7P/u/wgA+/8NAPj/DQDT/woAEgDf/wUA6P8AAPz/BQAoAPz/8v/f//T/+v8EAAMA8P/7//3///8xAPv/4f/g/9r/1v/9/wwAGQAHACQA+f8HAAIAFQADAPz///8hAAAAAAAFAOv//v8RAPL/4v/4/wsA6P/i////8P/6/9X/BAAHAAwACwDh/+r/8/8OAAkADAAI//3/DwD9/wYA/v/s/wEAHgAGAAsAHAD3/xQA+//+/+H/9P8RAPb/EQAKAAcABAD5/xEAHgACAP//AADo/9v/FgAPAPr/9v/0/w4A9P/0/wEABwAHABsA///j/wgA9P////n/CAD1/wcAAQDr/wIAwv/p//X/9f/s/wQA5f8IAPL/DgDV/wgABADz/wcA//8DAO3/9P8GAAcAEwDs/wAA2/8mAPr/6v/f//P/BwAPAAkAGgAMAPL/AgDJ/87/9v8BABAACQDB/w8ABQD8////BQD0//3/AAAFAPf/CADa/xMACgABAPr/+P/x/wsA9//4/wYA/f8NABsA7v/o////8P/x/wgA4//7/9r/3v8kAPf/FgDt/wUA7P8HAPP/t/9s/wEA7f/+//r/EAD8/wQA/f/v/+v/CQDo/wsA///2/x0A/f8AAAUAAADz//L/8v8AAA4ADQAPAPj/BQDt/x8Ayv/0/woA+f/0/+X/AwAKAOf/+//+/x0A7/8MAOX/AQAQAN7/AwDz////2v/1//T/7f/V//z/8//3////HwDv/+X//P8QAA0A+f/y/9b/4//c/wEAGwAFAAQAIwD5/w4AAgAOANP/AwADAAYA+P8AAAcA6f8BABEA6P/h/+3/DQC4/9j/+P/2//P/0P8sAAwADAD5/9P/5f/v/xAACQAIAD7/+f8HAP3/DAAAAAcA+P8iAAEAEQAEAPP/HwD//wAA9f/2/w8A2/8GAAAADQDs/wwAFgARAP7/AgD6//D/6v///xEABwDz/9f/CADz//L/AwD9/wgACwDh//D/BgDu/wAAAAAEAPD/AgDz/+X/+/+7/+j/AgD9/+n/+f/n//7/FwASAIz/CQDV/+r/9//l/wUA9P/y/wUA+/8KAOv/8//a//T/AQDg////+f8TABEADgANAAoA6f/4/8//Nv/2/wUA9f8QANX/AAANAPj/FgAIAAIAAAAAAAsAAQAaAOP/+/8ZAP///P/6/+D/EgD0//f/DAAOABUAFQDi/wcAAgAAAPD/GADz/xUAwv/1/ycAHwAfAPH/BwDw/wsAAACs/73/EwDt/wcAAAAOAPf/DgD9//L/zv/6//r/BgAjAPb/MQD0//z/BQAAAPr/BQD+/xUAHAAPABEA8v/u//T/KADG/wkAyv/3//n/1f8DAAEA4f/5/wAAEgD9/wwA7P/5/wgA4v8AAAsA9P/Z/wAACADr/+H//v8BAO///v8gAPH/7v/2/xIA/P/r//7/CwDh//T/+P8jABgAAgAiAOn/DgD7/wUA4v8MAPz/AAD7/wAAAwD1/xQAEgD6/+b/6f8QABL/1v/6//P/9P/U/wgADQAHAOP/7P/q//b/EQAPAAAAi/8AAAMAAAAVAAMAEAA7ACcAIAAPAO//8f/y/wkA+//w//P/CQDI//z/6P8QAOf/GQAWAAIA//8OAPL/EwDe/9j/FgD4//3/yv8GAOz/8/8OAPT/BgALAAsA8f8MAPn/BAD6/wgA6P8PAOr/+P8BALf/8f/w//7/7P/l/+f/DQADAPn/Ef8OANf/2f/u/6b/BwD4//P/6//w/wcA7P8CAOH/1P8JAOT/FQD5/xQAFQAVABcACADS/xYA0P8l//f/AgAFABMAs//9/wUA+f8IAAQA+v8AAPn/GAANACUA2/8qABcAFADy//v/4v8TAPL/AAD6/wcAGgAFAOz/HQACAAwA9P8UAB8AEQDU/wIAHQAiACQA6/8KAM//FAACAO//2P8QAPb/KAD+/xQA9v8KAPv/DwDZ/+v/7v8JACQA6/9DAO//+v8GAPX/AQAQAPz/KAAcABoAEAAGAPT/8P8BAM7/DgAEAPP/8f+4/xAACgDh//H/CgADAP7/BQDw//n/AgDf/wYAAwDj/7z//P8MAOf/sv8DAOv/9v8FADYA/v/s//f/JgDt/+//BQDv/+L/6f/0/w8A+f8IACgAzv9a//7/BADW/xEAAAANAP3//v8GAPD/AAAZAAQA0v/2/xIAG//R//X/9//6/+L/MwAMAAAA3f/u/87/+/8PAA8ACQC1//v/+//7/yQAAAAAACUAAwAkAA8A9f/p/wgAGAAOAPn/8/8EALv/6//c/xMA3f8ZABoA/v8WABIA9f8OAPT/lf8NAPb/AACF/wQA7v/y/w0A+P8LABAABgDt/xIA+/8AAO//+f/p/w8A5//4////x//x//7/CgDz/+L/8P8NAPj/7v9T/xQAxv/U/+H/bP8EAOz/7f8GAPD/BQDo/wAA4//H/wwA8v8bAP7/FwAOABUAGQAEAPP/BgC9/y3/+P/s/+L/GwBy//f/3v/4//H/CAAAAAAA//8YAB8AKQAIABIAIwAuAP3/8v/e/wkA8//0/wUAFQALAP7/7/8cAPv/AgDv/yMABQAXANH/DwDt/5D/5/////v/BwC1//X/o/8qABwACAADABwADAANAPL//v/9/yIA/f8RAPf/wP/g//r/7/8AAA8A4v/z/w8AAwDB/9D/7v8TAAcAFAANAOX/bf8RAB8APAD3/wAAHQAHAPj/MgD5/zEABgD5//v/GADr//L/AADx//H/AgDf/wQA/v/m/+L/vf8OAPL/1f8AAAYA+P+p/+H/DAAMABEA7f/p/wcAGQAAAPj/JgDy/+D/7v8AAPv/8v8GAAgA1P8UAAgA6v/o/xMA+f/s/xYAGwABABEABgDs/wIADwDf//L+AAAXABEACgALAPr/1P8DALT/7f/s//X/AAADAAEA+//z/xwAaP/6//f/CAD9/wkAAwD8//f/DwDz/9//6P8iAM//4P8UAPj/Qv8oAAkA4v8FAPr/8v/s/wUAGQDs////FQALABAAEQAIAPL/EgAHAI7/hP/4/wQA9f80AB8AEQDe//L/EQDm//b/6v/8/xcAg/8HAAoA2v8rAOH/GgANAAgA9//3/+7/BAAGAPv/CQAqAL//3f/v//r/8//o//z/DgAEABUAEwDp/93/CwD1//j/AADi//H/DgD0//v/4/8AAO///v8OAOn/GgDU//H/1P/c//n/CAD3//z/2P/k//r/AwDx//j/AgAZAMn/AADx/y4ADgDr/8D/8/8OAPn/mf/h//f/BwAfALL/9f+Z/yIAFAAHAAkABwAIAO3/5v/8/+3/GgAAAA4A+v+9/+r/9f/2/wMABwDm//P/BwAAAF//2v/u/w0ABQAKAC8A5P9z/+3/7P8iAPL/+v8CAPT/AAAVAAgAIQAGAAIA//8DAPT/1f////b/AQAAAPP/BgAAAPT/AADm/wUA9//j//f/EQD9/5f/5/8JABIAEwDw//X/5v8IAAkA+/8kAPb/9v/1/wQAIwDs/wkA7v/O/wMACADv//j/EgD0/+n/IQAZAB8AOgD+//v/8/8KANr/IP///x0AEAAAAP7/7//f/xoAuP/z/wQA9f/x/wMAAgD3/wAAFgDa//r/AAAFAPT/BAD5/93/7/8fAP7/6v/u/xYArf/p/wAA/v9D/xIADwDh/wIA//8BAOj/GwAWAO//GQDa/xEACAAQABIA6v8UAAQA1v+k/wcABADz/ysACAAKAMn/7f8LAOf/9v/0//H/FACg/wcA///i/woA8/8aABUABgD+//j/8f8AAA0A///4/xYA1v/g/+r/DADn//b/+/8FAAUAEQARAPv/5P/6//v//P/8/+v/7v8VAPH/+f/1/wAA9f/6/xQA6/8NANT/BgDk/+P/+v8OAPj/BgDf//P/AQAMAP3/6/8MAA8A0v/5//z/FgACAPr/xv8FAPf//v+s/+L/+/8KABEAvP/R/5n/EAAWAAsABwADAAwA7P/b//v/8v8aAPr/DwD7/8H/+v8BAPb/AwAEAOf/8f/z//z/lP/w//3/CQAAABIAJQDj/4v/BQAXACUA8/8NAPj/7/8AAB4A/P8lAA4AAwD9/ygA/v/d/wEA/P8IAAUA8f/8//7/8P8IAP//DgDq/+7/9f8PAPr/nv/2/xMABwAJAPL/9f/1//7/AQAAACIA9P/8//z/CgAdAPL/BwAEAMT/EQAPANL/8v8TAPT/1f8aACYAEQD5/wQA8v/9/xoA4v++/wIAFgARAAsA+v/8//X///+3//P//P/w//r//v8KAPT/+v8VAOn/AAD1////0f/v//D/yP///xQABwD6//T/EAA+/+7/GgAAAJX////7/+H/+/8HAP3/3v8nABQA8f8gAPT/AwALABkABgDr/xQA8v/l/7b/BQAFAPj/HQANAPP/yv/v/wQA7P/1//X/7v8NANf/DQAAAPL/IAAiABkACgAAAPT/+//t//H/EwD4/9n/+//q/93/8f8EAPH/3v/v//n/CQAPABAA8P/v//r//v/7/wQA8v/z/xAA8f/y/wIABwAAAPz/EQDz/xAA1f8AANT/3v/6/wwA9//4/+T/9v8EABEA/v/7/xQA+v/T//r/9v/1/wAA9P/Y//H/3v8IANH/7//3/w0AJQDO/9f/LP8JAA0AAwD5//z/DgD1/+X/5//y//b//f8BAAAAyf/z////9f8DAAMA8f/u/5z/BQDK//r/+f8QAPz/AwAeAP3/yP8HAN7/AwD2/wwA5f8cAAAADAAEABoADAAKAPr/KgAGAOf///8FAAoAAQD2/+z/AAD1/wEA/P8AAPP/9//x/xQA/f/R/xkACAACAOL/7//u//f/+f8UAAYAKQAAAPv//f8LACsA9f8DABIA1v8SAAsA5//8/xMA8P/X/wwA7v/6//P/BQDc//T/HgDu/+7/AQAVAP7/CQD5/+v/BgAPAIf/8v8LAOv/BQD+//P/8P/6/wYA/f8KAP/////9/tT/2//L/wYADQAFAOr/+f8OAH//+f8fAPn/2v/4/xYA5P/p/wsABADZ/xUAFAD3/woA1P8oAAcAIQDs/+n/EADj/+X/5P8KAPv/BAAUAPv/9P/J//T/GgDq/+3//f/n/xkA9f8hAOT/+/8IAPv/EQAAAP3/+P/9//f/+P8UAOz/zv/e//P/7//2//f/8/8AAOj/+f8mAAoAAAAGAO3/8//8//r////8/+3/EADt//r/FwAJAOz/+f8LAAUA7v/x/wAA+//3//n/3v/z//P/7f/2////FAAIAAoAGgDw/+L/9//8//3/CAD4/9v//v/m/x8A8/8EAPj/DAADAPf/2P+o/+f/DQAGAPj/9f8UAP7/6//O/+f/4v/4/8//AQDr//r//f8AAAAAAADy//P/dv8EAOH/AwABABQA/v8YAAAACQDp//n/DAD8//X/CAAAAPr//f8FAPv/HwAAAAYA6f8dAAoA3v/9/wUACAD6/wAA/P/4//X/DgAOAAgA9P/6//L/BAD+//j/OgAAAAMA1P/x/+3///8BAAsACAAsABAAAQAAAAgA7v/7/wYADwD3/wgAAQDu/w4AEgDm/9r/+f8gAMH/7//9//r//f8AAP3/BQAIAAoA8//6/+3///8JAAMAsP/3/wEA8f8QAPz/AwD4//3/+/8KAAkA/f/l/9v/4P/y/9v/AQD6/wUAAAD+/wEA1/8IABsA+//6//v/BADe/+3/CQAeANn/BAAMAPT///8OAA0ACgAVAAIA8P8MAN//5/8kAAIA9f8BAAgA7P/o/zQA8P/+/+//8P/9/+b/DwD//ykAh/8GABQAAwADAPP//v/7//j//v/x/w0A6//M/9//NgAAAPb/+//5/woA+f8BAA0ACQDk/wEA5//2//n////9/wQA8v8IAP7/+/8qABYAAgD3//n/CQBJ/w4ABQDg/wYA/v/6//f/+//2//D//f8UAAgADAAVAOv/+P/9//j//P8MAPj/6//y/9f/FwAcABcA7v8MAAIACgD4/3T/8P4GAPz/+//6/w4A8f/w/8r/5f/Q//n/t/8AAAYA/P8CAP3///8DAM7/8//u/+//CAATAPz/GgACAAYAAAAwAOT//f/S//b//f/+/w0AAADw//3///8QABEACADF/wYADQDj//r/AwAHAPn/+//t//T/8f8RAO7/AADz/xQA9f/N/wAAGwAWAPn/AwDh/+j/AAD2/yAABwAMACgAAgAQAPz/BAD2/wEA//8QAP7/AAD8//b/HQAUAOT/2P8OABgAtv/k/wEA9v/4/+7/FAALAAgA/P/m//n/+f8JAAsABgC9//T/8//1/xwAAAAEAPH/AAADAA4AAAD9//X/BwDY//b/+f8IAOL/AADy/wAA+P8FABEADwABAAIA7f8QAOf/zv8JABYA5P/0/wsA8v/7/wkA//8FABYAAAD0/wwA5v/t/y8ABADw//z//P/3/+//xP/x/wgA9P/w//D/6/8MADgAGgAi/wsA6v/w//X/3P8AAAAA+P8DAP7/BwDv/+r/5P/u/wgA7P/r//j/FQAEAAoAFwAEANz/AwDp/7H/+/8AAAUACwD5//7/+f/9/yYAWwAAAPb/AwAVAP3/IgDm/w4AFwABAPz/+P/t//7/8P/5/xQABQATABAA4/8YAPD//v/7/wsA+//4/wMA+P8dADcAIAD2/wsA2/8SAPv/yP+3/woA/f8AAPn/DgDt//r/2P/f/9P/9v+Z//7/GgD2/wQA+////wUAw//8/wYA9/8bABkABQAaAAUA+v/0/woAxf/7//v/8P/7/+n/FwANAOz/8//9/xEABAACAOv/CgANAOD///8HAAAA6f/3//3/6//n/wIAAgD8//T/HgD//9H/+v8pAAAA+f8MAN//8v8GAP3/EgADAAwAJgADAAYA/P///+v/BAAAAAQA+//2//7/9P8SABMA8v/c/9X/CwAD/+D/+f/6//3/2v80AA8ABADo//D/7P8BABUACQACAMX/8//e//j/FQD//wMA9f8AAAYAEQADAAAA9f8CAO3/8//2/wEA2f/2/+v/AwDp/xUAEAD1/wsABADz////6P9//wcABwDl/9//BADz//P/BQD2/wgAHAASAPz/DwDo//H/GgD9//L/AAD2//7/+/++//H/GgD4//X/8f/u/wsAAAABANr+DwDj/+v/5f+R/wEAAQDz/7n/9/8AAPL//P/o/+T/DAD7/wkAAAAaAAYADwD//wQA4v/5/+H/VP/+/wUABwAUAPX/+v/8/+7/CgAXAP//+P/6/xQACwAqAAUA+P8iAAEA8//2//P/AAD1/+X/EQAFABQACgDq/yoA+v/4////EgAAAPn//f8DAAoANQAcAO7/BgDU/xsA/v+s/+z/FQAAAAYA/P8KAPf/AgD4/+D/1v8AAMD/BQAXAOn/CwD5/wAABgDE//r/EQD2/y0AHwAIABcADAAAAPf/BwDI/wEA0f/7//j/4/8JABsA9v/y//v/+v8SAAYA+P/3/wMA2//+/wsA9P/m//H////r//D/GAD7//X/9P87AP7/zv/7/zEA3//3/xAAAAD8/wwA6/8IAA0ABgAqAPf/f//5//7/6v8LAP7/IQD4//P/AwD4/wwAFAD9/93/7P8LABr/3v/4//L/9//f/xUADAAEANH/BAAGAPj/EAAGABgAuv/z//7/+P8UAAUA+f/d//n/GAAKAPD/+P/+/woAAgD8//j//v+y//X/6f8HAN//HwARAPX/DwAVAO//9P/6/4L/AAD9//f/6v8FAPH//v/+//n/BQASAAYA9/8UAPr/7/8KAAEA8P/0//H/AAD+/6j/6/8YAPv/8//2//L/BAD9/+3/DP8RAOT/5//g/33/AAD3//b////0//n/7P8CAOb/2P8NAPr/HwD7/xYAEAANAA4AAQD8/wgA3v8l//n/BAAAABgA8//z//L/9v/4/wQAAwD3//r/EwAIACcA+f8sAA4ADwD4//L/6//4//T/3v8MAAkAFQD//+H/JwDz////+v8dACEA5//l/wkA+v/e/v3/+P/3/wYAAv///9P/JwALABAAAgAYAAIAAgDy//r//P8JAAYAAwD8/9T/4P8CAPP//f8SAK7//f8XAAUAFP/T/8f/JQASAP3/BQDq/2z/BQDv/xQA8/8HAA0A/f/4/x0A8P8PAPz/+P/8/+//6P/l////AQDr/+b/FgD9//H/JQD//8f/AAD0/+b/+P8MAAEArv/S/wEAEwArAAYA7/8lABoADADr/yUA9v/x/+//8P8SAPr/FQAAAA3/GQD9//b/+v8TAOn/5/8CAA0AIAAJAAQA9P/7//L/3v8z/wAAGAAcAPz/DAABAO7/DAD6//f/2//1/wQAMwAOAPP/9/8MAAL/CAAAAOb/AQD9//r/CwD4/xcA+//t/+7/FACn/4X/BwDj/zT/LQAIAPv/6v/P/wcA6v/1/xcA8f8TABsAHgAVABQAFQD3/xwADQBk/7j/BQAJAPj/NgAYAAMApv/s/x0A7v/5/+z//v8DAIb///8WAOT/FADn/w8ACgADAP//AAD8/wAAwP/r/wsARQDd//T/9v8NAA4Aqf8JAPr//f8bAAQA5P/g////DAAFABUA5v/G/xUA8f/3//L/8/8AAPv/IgDl/w8A3P/q/+P/4f/5/wcA7P8HAOD/7f/3/xYA3f8GAAIAEADb//7/7f8BAPz/9P/L/wQADADx/4z/AAD1/wQA//8r//j/xf8dABIAAAD8/w8ABwDh/+P/AAABAAoAAgAQAO//0v/2/wMA7v/9/wYAgv8AAAgA+P87/97/CAAaAA0A//8OAOH/Wf8CABEACAD1/wgADAAMAPz/PAAHABIA/f/9//z//v/v/+f/+P8BAPn/9v/q/wEA8//3//j/1//8//r/6P/7/wEA+/+a/97/CQAXABsACgD6//X/CgAIAPX/JwDy//L/9P/7/xMAAAAGAPz/Xf8MAAMA7v/8/xUA9f/l/wwAHAAxAEcA+v/v//3/HgDt/xP/+/8WABMAAwAFAND/7f8VAAkA9P/o/+r/7P/3//b/9P8CAA0A5f8CAPv/+//8/wcA/v87//r/IAD2/+f/8f8SAJf/5f8IAPT/OP8WABQA/P/q//f/DADp/x4AHgD2/xYACwALABAAFAAXAPX/GgD6/9X/yP/8//7/9v8oAA0A+f/I/+z/AADx//j/6v/2/w8ApP///wYA7P8NAOX/EgANAP3//P/9//P/+/8EAAMAAABFANX/5//z/xgA9f/i/wkAAgD3/xkACADs/+T/+f8DAAAA/v/o/9r/CgDv//z/8f/0/w8A+/8XAPP/DgDY//f/y//4//3/AQD0/wEA3f/w//z/BAD6/+7/+f8YANP/AADy//7/CQD6/9z/8/8CAP3/h/8CAPf/DQAVAIn/8f9+/w4ABAAJAPr/EQALAPT/5/8FAPr/CgACAA4A8v/N//b/AgDz/wMABQDJ/+n/9//5/4b/1//8/xEAEQAbAAoA9f+j/wQA5f8NAPP/AgD4/83/AgAbAAAABAABAP7/AAD7//X/4v/+/wsABAD//+//+v/w/w0A//////7/8f/u//j/BwAHAGj/5v8JAAwAJQD8/wAA+//s/wcA/v8pAP//9f/2/wAAGwAAAA8A9P/R/hQA/P/y/wAAFgDz/9//DwALAEMA+f/9//j///8RAOz/hP/9/xsAGAD+/wEAFAD4//3/BwDz//7/7P8CAPT//P/w/wIA+v/T/////P/5/+f/8v/3/yL/+P8lAAAA7//w/xMA8P7m/x0A8P9Z//n/EQD3/93/+v8NAN7/EwAcAPf/IgDu/yEADgAYABEA9P8aAO//1v++/wcABAD5/x4AAwDl/7T/7/8QAO7/9v/y//X/CgDX/wEAAADy/wYAGwATAAgA/v////z/9v/9/wsA//87/wYA6v/0//H/JgDy/+7/8f/6//z/FgATAAIA/v/6/wAA//8BAOz/2/8NAPL/9v8BAPH/BwD5/wwA+/8GANv/FQDt//P//v8GAPD/BQDf//T/AwAWAAEAAQAMAP//3f8DAAgAAgACAAUA4P8MAAAAAgC+//v/+/8SAAgAyP/y/5P/AgAUAA4ABAD6/woA///j/+7/8v/2/wAACQD5/97/+v8BAPH/BgAAAOb/+f9d//7/wP/6/w8AFAAPAAgA/f/7//z/AwAWAP7/+P8FAAAA7v8AAAkAAgDy/w4A/v8FAAMA/v/q/////P8DAAUA//8GAPH/CgANABAABAD4//P//P8OAP//v/8NAAEAEQAJAAIA8f8AAPP/AgADAC4ACAD7//n/AwAYAA8ACgAEAOL/HQD7//v/CAAUAPL/1v8RAC0A9v/+//7/6v8AADwA9//k/wEAEwD8//T//P/j/wEAHQD4//j/AADq//j//P8MAPf/+P/2//b/+//+//j/gP81//f/+f4AAAoA/v/q//T/DABp//P/EADz/87/9v8IAPb/zv8AABUA3v8dABQA+P8dAAoABwALABIAEQDx/xcA3v/W/xUAAgD8//7/FgAHAN3/zP/x//r/6v/3//3/8P8RAPf/EgDK//7/EwD2/w8A/P/7/////P/5//f/DgDz/07/9//t//7/8v/8//f/EQAOAPP/AQAOAPz/AwD8/wYA/v/+////9//9/xQA9f/5//3/DgD6//v/EgAEAAEA9f8IAOD/8v/+/wEA7//9/+b/9v/8/wYAAAAMAAwA+//p////AAAFAAwA9v/g/+3/+P8SAPf/CwD4/xEACwD0//H/lP/X/wgABgDz/w8ACgD5/+b/F//v/+z/8P/X//j/9//4/wIA/v8HAAAA1//x/3r/bgDw/woAAAAbAAYAEgAEAAQAEgANAOT//f/1/wEACwDe////+/8BAP//BwAAAP7/CQACAPL/+f8DAAIAAQD4//n/9v8FAB8ACAABAPf/9f/2/wkAAgAAACsAAgANAOj/+v8AAP3/EQD6/wYAMAA5APz/+v8AAAAAAAAJACMABQAHAAMA/P8YABYA4P/T//H/HwDD//z/AAD7//v/AgD3/wEAAgAPAAcA+v/u//7/DQAJAPr/+P8NAOr/CQADAAAA8v/8////AgD///3/AADu/y7/9//l////8f/+/+7/+f8MAMr//f8IAAIA+//3/woA9//I/wIAHQDc/wkAEQD5/wIADAAbAAkAEQD3//f/EwDc/9f/KQAHAPj///8MAAMAz//E/+//DgDw//P/6//u/w0A9v8oADv/BgDt//3/AgDv//v///8AAP3/+f8LAPH/HP/o/0EADgD9//D/9v8OAPf//P8LAAgA6/8aAAcAAAAAAAAAAgAAAAQADQD3/wAAAwAWABMA+P8GAAAA4v8RAAYABQAMAP7/4f/s////6f/w//X/AgD+/wgADQDp/wAA+P8EAP//AQACANz/MAD0/xoALgAQAPf/DADl/wcA+f+6/1v/BwALAPL/EAAJAPH/6/8j/+T/3P/9/37++f8HAP//+f/5/wAAAgCN//3/8P/+/xkAEgD//xoADAAGAP7/CAD5/wEABgD6//z/EQAGAA8A/v/3//r/AAAPAP7/9P8NAAMA7v/7/xkA//////v/9f/6//r/BwAKAAMA9f8NAPf/4v///yoAGAABABAA3//5/wsA/f8UAB0ACAAsAAsABAD6//z/9v/1/wIAGQAAAPf/+v/9/ygAFADo/9z/3P8IAMP+9/8BAPz//P/8/wMACwAJAP3/BwD6//7/EAAHAA8ABAD3//X/7v8PAP3////6//b//f8NAAoAAAD1/wYAL//9/wAA+//m//n/9v/2/wAA+f8IAA4ABQD9//T/EgDz/6j/AAAdANr/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+/8RAAUACADh//r/2v/n////AgDx/wcA0//z/wIA/v/v/xIABAACAOP/CgD8//7/BgD9/+T/9v8BAOz/p/8FAPP/FQAWALb/AAB9/wEACwAPAPn/JQAHAAIA8f////j/9P8AABUA+//0/wQA///1/wMAAQDb//j/Z//s/7v/8v8fABcAGAANAP//AQAxACAA7v8AAP7/CwAMAPH/AAD8/wwA+P8UAP7//P/x//z/8//7/wUAEQD//wAACgD5/xEAEgAVAP3/+v/q//X/BAAAAF7/AwD9/wYAFgAEAAQADAD+/xcAAQA4ABwA9P/y/wAAEgCb/xAACAD8/wIA/P///wgAEQDh/9f/CgAxAO7/CQD9//7/BgAGAPX/8/8FABYAEwD6//3/5f8BABQACQD2/wUA6P/v/wMABQDz//z/AQDo//P/AAD5//T+Tv8DAD7/+f/+//f/0v/u/xoAjf/v/wwA6f/C/wAA/v///9T/8v8LAOT/AQAaAPv/CAALAB4ADgAAABEA8P8aAO7/0//e/wcA+v/5/xIAEADF/7z/8v8CAOX/9//y//f/BgDl//f/vv/+//3/AwASAPH/+v////7/9v/8//7/+/9j/vf/7P8DAPb/EQD2/yQA///w//7/FAANABwAEAANAAMA//8FAPP/9v8MAPn//v/7//r/WgD8/wcA//8DAP3/BQDz//z/+f///+b/BADU//H/+v///w8ADAAGAP//9/8GAAwABQAIAP//4/8FAAIA+v/I/xEA+v8QAPf/9//+/9j/6P8IAPL/+P8hAA8A6f/x/0v/+f/v//z/8v/3//7///8AAPb/CAADAK7/+/9g//v/4/8EAAIAGwAeABQAAQAAAEEAFgD/////AAALABwABgAAAPj/CADp/xAA/P/1/wgA/f/y//3/EAALAAEA///0//b/CAAKABcA///6/+7/+P8HAAQAEgAgAPn/DgDz/wMADgAGAP7/CAADADUAIQDt//T//v/9//j/BQDn/x8A+v8FAPz/DwATAPj/2f8AABEA+v4IAAEA+P8FAAwA+P8FAAQAEQAQAOn/+//x/wcAFwAHAPf/AgDk//v//P8BAPD//v/w//v/AAAAAAEABgCE/vf////1/+z/9f/t/+//DADD//z/DQDp/+r//f8OAPX/4v/3/w4A5v8FABQA+/8MAAoA//8PAAAA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";
initOnce();
const rootNode = nodes[0];

if (nodeHost && process.argv.length > 2) {
  for (let i=2; i < process.argv.length; i++)
    uciExec(process.argv[i]);
  process.exit(0);
}

if (nodeHost) {

  process.stdin.setEncoding('utf8');

  process.stdin.on('readable', function() {
    const chunk = process.stdin.read();
    process.stdin.resume();
    if (chunk !== null) {
      uciExec(chunk);
    }
  });

  process.stdin.on('end', function() {
    process.exit();
  });

}

