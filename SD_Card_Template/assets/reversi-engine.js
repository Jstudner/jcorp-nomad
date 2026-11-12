/*
 * thaw-reversi-engine - Reversi rules + alpha-beta search.
 * Vendored from tom-weatherhead/thaw-reversi-engine, wrapped for the browser as
 * window.ThawReversi. Do not edit; re-vendor instead.
 *
 * Used by /Games/Reversi.html. API notes that are NOT obvious from the source:
 *   - tokens.white === 'X' and tokens.black === 'O'.
 *   - moveAutomatically(state, maxPly) returns {bestRow, bestColumn, gameState}.
 *     The next state is result.gameState, NOT the result itself.
 *   - moveManually(state, row, col) returns the next state directly, and reports
 *     numPiecesFlippedInLastMove === 0 for an illegal move rather than throwing.
 *   - createInitialState(board, player) coerces anything that is not 'O' to 'X'.
 *
 * Measured midgame search on desktop: ply 4 = 2 ms, ply 5 = 8 ms, ply 6 = 140 ms.
 * Strength is real - depth 5 beat depth 1 by +39 discs as black and -34 as white.
 *
 * MIT License
 *
 * Copyright (c) 2017 Tom Weatherhead
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

// thaw-reversi-engine/src/engine.js

'use strict';

const boardSize = 8;
const boardWidth = boardSize;
const boardHeight = boardWidth;	// Ensures that the board is square.

const eightDirections = [
	{ dx: -1, dy: -1 },
	{ dx:  0, dy: -1 },			// eslint-disable-line key-spacing
	{ dx:  1, dy: -1 },			// eslint-disable-line key-spacing
	{ dx: -1, dy:  0 },			// eslint-disable-line key-spacing
	{ dx:  1, dy:  0 },			// eslint-disable-line key-spacing
	{ dx: -1, dy:  1 },			// eslint-disable-line key-spacing
	{ dx:  0, dy:  1 },			// eslint-disable-line key-spacing
	{ dx:  1, dy:  1 }			// eslint-disable-line key-spacing
];

const tokens = {
	white: 'X',
	black: 'O',
	empty: ' '
};

function isDefined (arg) { // TODO: Import isDefined from thaw-common-utilities.js instead.
	return typeof arg !== 'undefined';
}

class Game {
	constructor (boardString) {
		this.boardWidth = boardWidth;
		this.boardHeight = boardHeight;
		this.boardArea = this.boardWidth * this.boardHeight;
		this.initialBestScore = -2 * this.boardArea;

		this.victoryScore = this.boardArea;

		if (!boardString) {
			boardString = Game.initialBoardAsString;
		}

		if (typeof boardString !== 'string') {
			throw new Error('boardString is not a string.');
		} else if (boardString.length !== this.boardArea) {
			throw new Error(`The length of boardString is not ${this.boardArea}.`);
		}

		this.directions = eightDirections;

		this.players = {};

		this.players[tokens.white] = {
			piecePopulation: (boardString.match(/X/g) || []).length,
			token: tokens.white
		};
		this.players[tokens.black] = {
			piecePopulation: (boardString.match(/O/g) || []).length,
			token: tokens.black
		};

		// Split a string into an array of characters:
		// see https://stackoverflow.com/questions/4547609/how-do-you-get-a-string-to-a-character-array-in-javascript/34717402#34717402
		// this.boardArray = boardString.split('');
		this.boardArray = [...boardString];
		// this.boardArray = Array.from(boardString);

		// Note: the following lines construct a circular data structure.
		this.players[tokens.white].opponent = this.players[tokens.black];
		this.players[tokens.black].opponent = this.players[tokens.white];
	}

	getBoardAsString () {
		return this.boardArray.reduce(
			(accumulator, element) => accumulator.concat(element),
			[]
		).join('');
	}

	getSquareState (row, column) {

		if (row < 0 || row >= this.boardHeight || column < 0 || column >= this.boardWidth) {
			// throw new Error('getSquareState() : Coordinates are off the board.');

			return null;
		}

		return this.boardArray[row * this.boardWidth + column];
	}

	setSquareState (row, column, token) {

		if (row < 0 || row >= this.boardHeight || column < 0 || column >= this.boardWidth) {
			throw new Error('setSquareState() : Coordinates are off the board.');
		}

		this.boardArray[row * this.boardWidth + column] = token;
	}

	squareScore (row, column) {		// Calculate a useful heuristic.
		const interiorSquareScore = 1;
		const edgeSquareScore = 2;
		const cornerSquareScore = 8;

		const isInEdgeColumn = column === 0 || column === this.boardWidth - 1;

		if (row === 0 || row === this.boardHeight - 1) {

			if (isInEdgeColumn) {
				return cornerSquareScore;
			} else {
				return edgeSquareScore;
			}
		} else if (isInEdgeColumn) {
			return edgeSquareScore;
		} else {
			return interiorSquareScore;
		}
	}

	placePiece (player, row, column) {
		const returnObject = {
			score: 0,
			flippedPieces: []
		};

		if (row < 0 || row >= this.boardHeight || column < 0 || column >= this.boardWidth ||
			this.getSquareState(row, column) !== tokens.empty) {

			return returnObject;
		}

		this.directions.forEach(direction => {
			// Pass 1: Scan.

			let canFlipInThisDirection = null;
			const undoBuffer = [];
			let row2 = row;
			let column2 = column;

			for (;;) {
				row2 += direction.dy;
				column2 += direction.dx;

				if (canFlipInThisDirection !== null || row2 < 0 || row2 >= this.boardHeight || column2 < 0 || column2 >= this.boardWidth) {
					break;
				}

				const squareState = this.getSquareState(row2, column2);

				if (squareState === player) {
					canFlipInThisDirection = true;
				} else if (squareState === tokens.empty) {
					canFlipInThisDirection = false;
				} else { // squareState === opponentToken
					undoBuffer.push({ row: row2, column: column2 });
				}
			}

			if (canFlipInThisDirection) {
				returnObject.flippedPieces = returnObject.flippedPieces.concat(undoBuffer);
			}
		});

		returnObject.numPiecesFlipped = returnObject.flippedPieces.length;

		if (returnObject.numPiecesFlipped) {
			// Pass 2: Flip.

			returnObject.flippedPieces.forEach(coord => {
				this.setSquareState(coord.row, coord.column, player);
				returnObject.score += 2 * this.squareScore(coord.row, coord.column);
			});

			this.setSquareState(row, column, player);
			returnObject.score += this.squareScore(row, column);
			this.players[player].piecePopulation += returnObject.numPiecesFlipped + 1;
			this.players[player].opponent.piecePopulation -= returnObject.numPiecesFlipped;
		}
		// Else no opposing pieces were flipped, and the move fails.

		return returnObject;
	}

	findBestMove (
		player, nPly,
		nParentScore = 0, nBestUncleRecursiveScore) {	// nParentScore and nBestUncleRecursiveScore are for alpha-beta pruning.

		if (!isDefined(nBestUncleRecursiveScore)) {
			nBestUncleRecursiveScore = this.initialBestScore;
		}

		const returnObject = {
			bestRow: -1,
			bestColumn: -1 //,
			// numberOfLegalMoves: 0
		};
		const opponent = this.players[player].opponent.token;
		let nBestScore = this.initialBestScore;
		let bestMoves = [];
		let doneSearching = false;

		for (let row = 0; row < this.boardHeight && !doneSearching; ++row) {

			for (let column = 0; column < this.boardWidth; ++column) {
				const placePieceResult = this.placePiece(player, row, column);
				const numPiecesFlipped = placePieceResult.flippedPieces.length;

				if (!numPiecesFlipped) {
					// Is the "continue" keyword "bad" in JavaScript?
					// See e.g. https://stackoverflow.com/questions/11728757/why-are-continue-statements-bad-in-javascript
					continue;			// eslint-disable-line no-continue
				}

				// returnObject.numberOfLegalMoves++;

				let nScore = placePieceResult.score;

				if (this.players[player].opponent.piecePopulation === 0) {
					// The opposing player has been annihilated.
					nScore = this.victoryScore;
				} else if (nPly > 1 &&
					this.players[tokens.white].piecePopulation + this.players[tokens.black].piecePopulation < this.boardArea) {

					const childReturnObject = this.findBestMove(opponent, nPly - 1, nScore, nBestScore);

					nScore -= childReturnObject.bestScore;
				}

				this.setSquareState(row, column, tokens.empty);

				placePieceResult.flippedPieces.forEach(squareCoordinates => {
					this.setSquareState(squareCoordinates.row, squareCoordinates.column, opponent);
				});

				this.players[player].piecePopulation -= numPiecesFlipped + 1;
				this.players[player].opponent.piecePopulation += numPiecesFlipped;

				if (nScore > nBestScore) {
					nBestScore = nScore;
					bestMoves = [];
					bestMoves.push({ row: row, column: column });

					if (nParentScore - nBestScore < nBestUncleRecursiveScore) {
						// *** Here is where the alpha-beta pruning happens ****
						// Because of the initial parameters for the top-level move, this break is never executed for the top-level move.
						doneSearching = true;
						break; // ie. return.
					}
				} else if (nScore === nBestScore) {
					bestMoves.push({ row: row, column: column });
				}
			}
		}

		// bestMoves.sort((move1, move2) => {

		// 	if (move1.row !== move2.row) {
		// 		return move1.row - move2.row;
		// 	} else {
		// 		return move1.column - move2.column;
		// 	}
		// });

		if (bestMoves.length) { // I.e. if (returnObject.numberOfLegalMoves === 0) {
			const j = Math.floor(Math.random() * bestMoves.length);
			const selectedBestMove = bestMoves[j];

			returnObject.bestRow = selectedBestMove.row;
			returnObject.bestColumn = selectedBestMove.column;
		} else {
			nBestScore = 0;
		}

		returnObject.bestScore = nBestScore;
		returnObject.bestMoves = bestMoves;

		return returnObject;
	}

	noLegalMovesForPlayer (player) {
		const result = this.findBestMove(player, 1);

		// return result.numberOfLegalMoves === 0; // Or:

		return result.bestRow < 0;	// bestColumn would work as well as bestRow
	}

	isGameDeadlocked () {
		return this.noLegalMovesForPlayer(tokens.white) && this.noLegalMovesForPlayer(tokens.black);
	}

	isGameNotOver () {
		return this.players[tokens.white].piecePopulation > 0 &&
			this.players[tokens.black].piecePopulation > 0 &&
			this.players[tokens.white].piecePopulation + this.players[tokens.black].piecePopulation < this.boardArea &&
			!this.isGameDeadlocked();
	}
}

Game.createInitialBoardString = () => {
	// boardString = emptySquareToken x this.boardArea;
	const boardArray = [];

	for (let i = 0; i < boardWidth * boardHeight; i++) {
		boardArray.push(tokens.empty);
	}

	const halfWidth = Math.floor(boardWidth / 2);
	const halfHeight = Math.floor(boardHeight / 2);

	// JavaScript strings are immutable.
	// See https://stackoverflow.com/questions/1431094/how-do-i-replace-a-character-at-a-particular-index-in-javascript
	// Change boardArray before the join() instead of trying to change the resulting string after.
	boardArray[(halfHeight - 1) * boardWidth + halfWidth - 1] = tokens.white;
	boardArray[(halfHeight - 1) * boardWidth + halfWidth] = tokens.black;
	boardArray[halfHeight * boardWidth + halfWidth - 1] = tokens.black;
	boardArray[halfHeight * boardWidth + halfWidth] = tokens.white;

	return boardArray.join('');
};

Game.initialBoardAsString = Game.createInitialBoardString();

// TODO: Pass an optional 'descriptor = {}' parameter? See avoidwork's filesize.js

function findBestMove (boardString, player, maxPly) {
	const game = new Game(boardString); // Use a temporary game object?

	return game.findBestMove(player, maxPly);
}

// BEGIN Version 0.2.0 API

function getPopulations (game) {
	const result = {};

	result[tokens.white] = game.players[tokens.white].piecePopulation;
	result[tokens.black] = game.players[tokens.black].piecePopulation;

	return result;
}

function createInitialState (boardAsString, player) {
	boardAsString = boardAsString || Game.initialBoardAsString;

	const game = new Game(boardAsString);

	return {
		game: game,
		boardAsString: boardAsString,
		populations: getPopulations(game),
		player: player === 'O' ? player : 'X',
		isGameOver: false
	};
}

function moveManually (gameState, row, column) {
	const resultOfPlacePiece = gameState.game.placePiece(gameState.player, row, column);

	return {
		game: gameState.game,
		boardAsString: gameState.game.getBoardAsString(),
		populations: getPopulations(gameState.game),
		player: gameState.player === tokens.white ? tokens.black : tokens.white,
		isGameOver: !gameState.game.isGameNotOver(),
		numPiecesFlippedInLastMove: resultOfPlacePiece.numPiecesFlipped
	};
}

function moveAutomatically (gameState, maxPly) {
	const result = findBestMove(gameState.boardAsString, gameState.player, maxPly);

	result.gameState = moveManually(gameState, result.bestRow, result.bestColumn);

	return result;
}

function getURLFriendlyBoardStringFromGameState (gameState) {
	return gameState.game.getBoardAsString().replace(/ /g, 'E');
}

// END Version 0.2.0 API


window.ThawReversi = {
  Game: Game,
  boardSize: boardSize,
  tokens: tokens,
  createInitialBoard: function(){ return Game.initialBoardAsString; },
  findBestMove: findBestMove,
  createInitialState: createInitialState,
  moveManually: moveManually,
  moveAutomatically: moveAutomatically
};
