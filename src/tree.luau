-- tree: дерево вкладки с контролем лимита Porthole.
--
-- Porthole отклоняет дерево целиком, если в нём больше 4096 байт текста, и вкладка замирает на
-- последней удачной отрисовке (лаба, lab 0.2). Поэтому размер считается заранее, а не по ошибке:
-- билдер принимает элементы, пока они влезают в бюджет, а вместо остальных ставит одну строку
-- «не всё поместилось». Кириллица — 2 байта на символ.
--
-- Считаем с запасом: все строки элемента, включая вид («status», «text»), id и подпись кнопки.
-- Porthole, скорее всего, считает меньше, но лишний запас безопаснее недостающего.
local tree = {}
tree.LIMIT = 4096
tree.BUDGET = 3800   -- остаток — запас под строку «не всё поместилось»
tree.LINE_MAX = 300  -- байт на одну строку текста

-- размер дерева или элемента в байтах текста
local function cost(node)
	if type(node) == "string" then return #node end
	if type(node) ~= "table" then return 0 end
	local n = 0
	for _, v in pairs(node) do
		if type(v) == "string" or type(v) == "table" then n += cost(v) end
	end
	return n
end
tree.cost = cost

-- строка для показа: без управляющих символов и «stack traceback», валидный UTF-8, не длиннее max байт
function tree.line(s, max)
	max = max or tree.LINE_MAX
	s = tostring(s)
	s = string.gsub(s, "%s*stack traceback:.*$", "")
	s = string.gsub(s, "[%c]", " ")
	if utf8.len(s) == nil then
		-- битые байты заменяем, а не выбрасываем строку целиком
		s = string.gsub(s, "[\128-\255]", "?")
	end
	if #s <= max then return s end
	local keep = 0
	for pos, code in utf8.codes(s) do
		local stop = pos - 1 + #utf8.char(code)
		if stop > max - 3 then break end -- 3 байта на «…»
		keep = stop
	end
	return string.sub(s, 1, keep) .. "…"
end

function tree.button(id, label, on_press)
	return { "button", id = id, label = tree.line(label, 80), on_press = on_press }
end

local B = {}
B.__index = B

-- more_text: строка вместо того, что не влезло (переведённая)
function tree.new(more_text, budget)
	local self = setmetatable({}, B)
	self.items = {}
	self.used = 0
	self.budget = budget or tree.BUDGET
	self.cut = false
	self.more = { "text", tree.line(more_text or "…", 120), muted = true }
	assert(self.budget + cost(self.more) <= tree.LIMIT, "tree budget leaves no room for the overflow line")
	return self
end

-- добавить элемент, если влезает; после первого невлезшего остальные тоже отбрасываются,
-- чтобы вкладка не показывала «дырявый» список
function B:add(node)
	if self.cut then return false end
	local c = cost(node)
	if self.used + c > self.budget then
		self.cut = true
		return false
	end
	self.items[#self.items + 1] = node
	self.used += c
	return true
end

function B:status(kind, text) return self:add({ "status", kind, tree.line(text) }) end
function B:text(text, muted) return self:add({ "text", tree.line(text), muted = muted == true or nil }) end
function B:gap() return self:add({ "text", " " }) end -- отдельного отступа в Porthole нет
function B:row(...) return self:add({ "row", ... }) end

-- готовое дерево для porthole.ui.render и его размер
function B:build()
	local out = table.clone(self.items)
	if self.cut then out[#out + 1] = self.more end
	return out, cost(out)
end

return tree
