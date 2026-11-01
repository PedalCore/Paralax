-- Tetris, built entirely through editor commands by tools/agent/examples/tetris/build_tetris.py.
--
-- Controls: Left/Right move, Up rotates, Down drops faster, Space drops at once. Clearing 1-4 rows scores
-- 100/300/500/800 times the level; every 10 rows the level rises and pieces fall faster. When a new piece has no room
-- the game is over, and the scene restarts after two seconds; the best score is kept in Game across restarts.
--
-- The board is a grid of cells (x 0-9 left to right, y 0-19 top to bottom). Each settled cell is a cube entity.
-- MATERIALS is replaced by the builder with the handles of the seven piece materials.
local Materials = MATERIALS

local Width, Height = 10, 20
local Names = { "I", "O", "T", "S", "Z", "J", "L" }
-- Cells of each piece in its spawn rotation, inside a square box of side Size (row 0 at the top).
local Shapes = {
	I = { Size = 4, Cells = { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } } },
	O = { Size = 2, Cells = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } } },
	T = { Size = 3, Cells = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } } },
	S = { Size = 3, Cells = { { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } } },
	Z = { Size = 3, Cells = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } } },
	J = { Size = 3, Cells = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } } },
	L = { Size = 3, Cells = { { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } } },
}
local LineScores = { 100, 300, 500, 800 }
-- Wall kicks tried when a rotation does not fit where it is.
local Kicks = { 0, -1, 1, -2, 2 }
local RepeatDelay, RepeatInterval = 0.17, 0.05

local Tetris = {}

-- Cells of a piece in rotation r (0-3), turned clockwise about the centre of its box.
local function CellsOf(name, rotation)
	local shape = Shapes[name]
	local cells = {}
	for i, cell in ipairs(shape.Cells) do
		local x, y = cell[1], cell[2]
		for _ = 1, rotation % 4 do
			x, y = shape.Size - 1 - y, x
		end
		cells[i] = { x, y }
	end
	return cells
end

local function WorldPosition(x, y)
	return vec3(x, Height - 1 - y, 0)
end

local function MakeBlock(name)
	local block = Scene.CreateEntity("Block")
	local mesh = block:AddComponent("Mesh", { Mesh = "1" })
	mesh.Materials = { Materials[name] }
	block:GetComponent("Transform").Scale = vec3(0.92, 0.92, 0.92)
	return block
end

function Tetris:OnCreate()
	TetrisGame = self -- For script.eval: inspect and drive the game in tests.
	self.board = {} -- board[y][x] = block entity, or nil
	for y = 0, Height - 1 do
		self.board[y] = {}
	end
	self.score, self.lines, self.level = 0, 0, 1
	self.fall, self.over, self.overTime = 0, false, 0
	self.bag = {}
	self.next = self:Draw()
	self:Spawn()
end

-- The next piece from a shuffled bag of all seven, so no piece is ever long in coming. math.random is seeded, so the
-- sequence is the same on every run.
function Tetris:Draw()
	if #self.bag == 0 then
		for i, name in ipairs(Names) do
			self.bag[i] = name
		end
		for i = #self.bag, 2, -1 do
			local j = math.random(i)
			self.bag[i], self.bag[j] = self.bag[j], self.bag[i]
		end
	end
	return table.remove(self.bag)
end

function Tetris:Fits(name, rotation, px, py)
	for _, cell in ipairs(CellsOf(name, rotation)) do
		local x, y = px + cell[1], py + cell[2]
		if x < 0 or x >= Width or y >= Height or (y >= 0 and self.board[y][x]) then
			return false
		end
	end
	return true
end

function Tetris:Spawn(name)
	self.piece = { name = name or self.next, rotation = 0, x = 3, y = 0, blocks = {} }
	if not name then
		self.next = self:Draw()
	end
	for i = 1, 4 do
		self.piece.blocks[i] = MakeBlock(self.piece.name)
	end
	if not self:Fits(self.piece.name, 0, self.piece.x, self.piece.y) then
		self.over = true
		local best = math.max(Game.Get("BestScore", 0), self.score)
		Game.Set("BestScore", best)
		Game.Set("GamesPlayed", Game.Get("GamesPlayed", 0) + 1)
		Log.Info("Tetris: game over with " .. self.score .. " points (best " .. best .. ")")
	end
	self:Place()
end

-- Moves the active piece's blocks to where the piece is.
function Tetris:Place()
	local piece = self.piece
	for i, cell in ipairs(CellsOf(piece.name, piece.rotation)) do
		piece.blocks[i]:GetComponent("Transform").Translation = WorldPosition(piece.x + cell[1], piece.y + cell[2])
	end
end

function Tetris:Move(dx, dy)
	local piece = self.piece
	if self:Fits(piece.name, piece.rotation, piece.x + dx, piece.y + dy) then
		piece.x, piece.y = piece.x + dx, piece.y + dy
		self:Place()
		return true
	end
	return false
end

function Tetris:Rotate()
	local piece = self.piece
	local rotation = (piece.rotation + 1) % 4
	for _, kick in ipairs(Kicks) do
		if self:Fits(piece.name, rotation, piece.x + kick, piece.y) then
			piece.rotation, piece.x = rotation, piece.x + kick
			self:Place()
			return true
		end
	end
	return false
end

-- Settles the active piece into the board, clears full rows and brings in the next piece.
function Tetris:Lock()
	local piece = self.piece
	for i, cell in ipairs(CellsOf(piece.name, piece.rotation)) do
		local x, y = piece.x + cell[1], piece.y + cell[2]
		if y >= 0 then
			self.board[y][x] = piece.blocks[i]
		else
			piece.blocks[i]:Destroy()
		end
	end
	local cleared = 0
	local y = Height - 1
	while y >= 0 do
		local full = true
		for x = 0, Width - 1 do
			full = full and self.board[y][x] ~= nil
		end
		if full then
			for x = 0, Width - 1 do
				self.board[y][x]:Destroy()
			end
			-- Everything above falls one row.
			for above = y, 1, -1 do
				for x = 0, Width - 1 do
					local block = self.board[above - 1][x]
					self.board[above][x] = block
					if block then
						block:GetComponent("Transform").Translation = WorldPosition(x, above)
					end
				end
			end
			self.board[0] = {}
			cleared = cleared + 1
		else
			y = y - 1
		end
	end
	Audio.PlayOneShot(cleared > 0 and "Audio/Clear.wav" or "Audio/Drop.wav")
	if cleared > 0 then
		self.score = self.score + LineScores[cleared] * self.level
		self.lines = self.lines + cleared
		self.level = 1 + self.lines // 10
		Log.Info("Tetris: " .. cleared .. " row(s), score " .. self.score .. ", level " .. self.level)
	end
	self:Spawn()
end

function Tetris:HardDrop()
	while self:Move(0, 1) do
	end
	self:Lock()
end

-- A held key moves once at once, then repeatedly after a short delay.
function Tetris:Repeat(key, dt, action)
	if Input.WasKeyPressed(key) then
		self.held = { key = key, time = 0 }
		action()
	elseif self.held and self.held.key == key and Input.IsKeyDown(key) then
		self.held.time = self.held.time + dt
		while self.held.time >= RepeatDelay + RepeatInterval do
			self.held.time = self.held.time - RepeatInterval
			action()
		end
	end
end

function Tetris:OnUpdate(dt)
	if self.over then
		self.overTime = self.overTime + dt
		if self.overTime >= 2 then
			Scene.Reload()
		end
		return
	end
	self:Repeat(Key.Left, dt, function() self:Move(-1, 0) end)
	self:Repeat(Key.Right, dt, function() self:Move(1, 0) end)
	if Input.WasKeyPressed(Key.Up) then
		self:Rotate()
	end
	if Input.WasKeyPressed(Key.Space) then
		self:HardDrop()
		return
	end
	local interval = Input.IsKeyDown(Key.Down) and 0.05 or math.max(0.1, 0.8 - (self.level - 1) * 0.07)
	self.fall = self.fall + dt
	while self.fall >= interval and not self.over do
		self.fall = self.fall - interval
		if not self:Move(0, 1) then
			self:Lock()
			self.fall = 0
		end
	end
end

-- Test hooks (script.eval): set up a position directly.
function Tetris:SetRow(y, pattern)
	for x = 0, Width - 1 do
		if self.board[y][x] then
			self.board[y][x]:Destroy()
			self.board[y][x] = nil
		end
		local name = pattern:sub(x + 1, x + 1)
		if name ~= "." then
			local block = MakeBlock(name)
			block:GetComponent("Transform").Translation = WorldPosition(x, y)
			self.board[y][x] = block
		end
	end
end

function Tetris:SetPiece(name, rotation, x, y)
	for _, block in ipairs(self.piece.blocks) do
		block:Destroy()
	end
	self:Spawn(name)
	self.piece.rotation, self.piece.x, self.piece.y = rotation, x, y
	self:Place()
end

function Tetris:BlockCount()
	local count = 0
	for y = 0, Height - 1 do
		for x = 0, Width - 1 do
			count = count + (self.board[y][x] and 1 or 0)
		end
	end
	return count
end

return Tetris
