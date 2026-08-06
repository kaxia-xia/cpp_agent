#!/usr/bin/env python3
"""🐍 AI 贪吃蛇 — 图片帧版，最后合成 GIF！"""

import os, sys, time, random, math
from collections import deque
from PIL import Image, ImageDraw, ImageFont

# ── 游戏配置 ────────────────────────────────────────────
COLS, ROWS = 30, 20          # 格子数
CELL = 24                    # 每格像素
W, H = COLS * CELL, ROWS * CELL + 40  # 底部留状态栏

DIRS = {'U': (0, -1), 'D': (0, 1), 'L': (-1, 0), 'R': (1, 0)}
REVERSE = {'U': 'D', 'D': 'U', 'L': 'R', 'R': 'L'}

# 颜色
BG      = (25, 25, 40)
WALL    = (60, 60, 100)
SNAKE_H = (100, 255, 100)
SNAKE_B = (60, 200, 60)
FOOD    = (255, 100, 150)
TEXT_C  = (255, 220, 100)
DANGER  = (255, 60, 60)

class SnakeGame:
    def __init__(self):
        self.snake = deque()
        cx, cy = COLS // 2, ROWS // 2
        for i in range(4):
            self.snake.append((cx - i, cy))
        self.dir = 'R'
        self.food = None
        self.score = 0
        self.steps = 0
        self.alive = True
        self.message = "喵~ 咱来啦！🐍"
        self.spawn_food()

    def spawn_food(self):
        occupied = set(self.snake)
        empty = [(x, y) for x in range(1, COLS-1) for y in range(1, ROWS-1)
                 if (x, y) not in occupied]
        self.food = random.choice(empty) if empty else None

    def step(self, direction):
        self.steps += 1
        self.dir = direction
        hx, hy = self.snake[0]
        dx, dy = DIRS[direction]
        nx, ny = hx + dx, hy + dy

        if nx <= 0 or nx >= COLS-1 or ny <= 0 or ny >= ROWS-1:
            self.alive = False
            self.message = "💀 撞墙了喵…"
            return False, False

        body = set(self.snake)
        ate = (nx, ny) == self.food
        if not ate:
            body.discard(self.snake[-1])
        if (nx, ny) in body:
            self.alive = False
            self.message = "😵 咬到自己了…"
            return False, False

        self.snake.appendleft((nx, ny))
        if ate:
            self.score += 1
            self.spawn_food()
            if self.food is None:
                self.alive = False
                self.message = "🎉 通关！！地图填满了！！"
            else:
                msgs = ["啊呜~ 好吃！😋", "这颗♥也是我的！", "美味！", "再来一颗！", "嘿嘿~"]
                self.message = random.choice(msgs)
        else:
            self.snake.pop()
            if random.random() < 0.08:
                msgs = ["小心小心…", "啦啦啦~", "冲鸭！！", "emmm...绕一下",
                        "咱好厉害对不对！", "完美路线！", "吃吃吃！", "哼~难不倒我"]
                self.message = random.choice(msgs)
            else:
                self.message = ""

        return True, ate

# ── AI (BFS) ────────────────────────────────────────────
def bfs_path(start, target, snake_set, w, h):
    if start == target:
        return None
    visited = set()
    queue = deque()
    for d, (dx, dy) in DIRS.items():
        nx, ny = start[0]+dx, start[1]+dy
        if (nx, ny) not in snake_set and 0 < nx < w-1 and 0 < ny < h-1:
            queue.append((nx, ny, d))
            visited.add((nx, ny))
    while queue:
        cx, cy, fd = queue.popleft()
        if (cx, cy) == target:
            return fd
        for d, (dx, dy) in DIRS.items():
            nx, ny = cx+dx, cy+dy
            if (nx, ny) not in visited and (nx, ny) not in snake_set \
               and 0 < nx < w-1 and 0 < ny < h-1:
                visited.add((nx, ny))
                queue.append((nx, ny, fd))
    return None

def ai_decide(game):
    head = game.snake[0]
    food = game.food
    if not food:
        return game.dir
    body_set = set(game.snake)
    body_set.discard(game.snake[-1])
    path_dir = bfs_path(head, food, body_set, COLS, ROWS)
    if path_dir:
        dx, dy = DIRS[path_dir]
        nx, ny = head[0]+dx, head[1]+dy
        if (nx, ny) == food:
            if len(game.snake) > COLS * ROWS * 0.5:
                tail = game.snake[-1]
                fd = bfs_path(head, tail, body_set, COLS, ROWS)
                if fd and fd != REVERSE.get(game.dir):
                    return fd
            return path_dir
        future_body = set(game.snake)
        future_body.discard(game.snake[-1])
        future_body.add((nx, ny))
        fp = bfs_path((nx, ny), food, future_body, COLS, ROWS)
        if fp:
            return path_dir
    tail = game.snake[-1]
    td = bfs_path(head, tail, body_set, COLS, ROWS)
    if td and td != REVERSE.get(game.dir):
        return td
    safe = []
    for d, (dx, dy) in DIRS.items():
        if d == REVERSE.get(game.dir):
            continue
        nx, ny = head[0]+dx, head[1]+dy
        if (nx, ny) not in body_set and 0 < nx < COLS-1 and 0 < ny < ROWS-1:
            safe.append(d)
    return random.choice(safe) if safe else game.dir

# ── 渲染单帧 ────────────────────────────────────────────
def render_frame(game, frame_idx, out_dir):
    img = Image.new("RGB", (W, H), BG)
    draw = ImageDraw.Draw(img)

    # 网格线
    for x in range(COLS):
        draw.line([(x*CELL, 0), (x*CELL, ROWS*CELL)], fill=(35, 35, 55), width=1)
    for y in range(ROWS):
        draw.line([(0, y*CELL), (COLS*CELL, y*CELL)], fill=(35, 35, 55), width=1)

    # 墙壁
    for x in range(COLS):
        draw.rectangle([x*CELL, 0, x*CELL+CELL-1, CELL-1], fill=WALL)
        draw.rectangle([x*CELL, (ROWS-1)*CELL, x*CELL+CELL-1, (ROWS-1)*CELL+CELL-1], fill=WALL)
    for y in range(ROWS):
        draw.rectangle([0, y*CELL, CELL-1, y*CELL+CELL-1], fill=WALL)
        draw.rectangle([(COLS-1)*CELL, y*CELL, (COLS-1)*CELL+CELL-1, y*CELL+CELL-1], fill=WALL)

    # 食物（带光晕脉冲）
    if game.food:
        fx, fy = game.food
        cx, cy = fx*CELL + CELL//2, fy*CELL + CELL//2
        pulse = 0.7 + 0.3 * math.sin(game.steps * 0.5)
        r = int(CELL//2 * pulse)
        # 光晕
        for i in range(3, 0, -1):
            alpha = int(60 / i)
            draw.ellipse([cx-r-i*2, cy-r-i*2, cx+r+i*2, cy+r+i*2],
                         fill=(255, 100, 150, alpha) if hasattr(draw, 'RGBA') else FOOD)
        # 主体
        draw.ellipse([cx-r+1, cy-r+1, cx+r-1, cy+r-1], fill=FOOD)
        # 高光
        draw.ellipse([cx-r//2, cy-r//2, cx, cy], fill=(255, 200, 210))

    # 蛇身
    for i, (sx, sy) in enumerate(game.snake):
        x1, y1 = sx*CELL + 2, sy*CELL + 2
        x2, y2 = sx*CELL + CELL - 3, sy*CELL + CELL - 3
        color = SNAKE_H if i == 0 else SNAKE_B
        radius = 5
        draw.rounded_rectangle([x1, y1, x2, y2], radius=radius, fill=color)
        # 蛇头眼睛
        if i == 0:
            d = game.dir
            ex, ey = CELL//2, CELL//2
            if d == 'R': e1 = (x1+10, y1+5); e2 = (x1+10, y1+11)
            elif d == 'L': e1 = (x1+4, y1+5); e2 = (x1+4, y1+11)
            elif d == 'U': e1 = (x1+5, y1+4); e2 = (x1+11, y1+4)
            else: e1 = (x1+5, y1+10); e2 = (x1+11, y1+10)
            draw.ellipse([e1[0]-2, e1[1]-2, e1[0]+2, e1[1]+2], fill=(255, 255, 255))
            draw.ellipse([e2[0]-2, e2[1]-2, e2[0]+2, e2[1]+2], fill=(255, 255, 255))
            # 瞳孔
            draw.ellipse([e1[0]-1, e1[1]-1, e1[0]+1, e1[1]+1], fill=(0, 0, 0))
            draw.ellipse([e2[0]-1, e2[1]-1, e2[0]+1, e2[1]+1], fill=(0, 0, 0))

    # 状态栏
    bar_y = ROWS * CELL + 5
    draw.text((8, bar_y), f"🍎 {game.score:03d}  |  🐍 {len(game.snake):03d}  |  步数 {game.steps}",
              fill=TEXT_C)
    if game.message:
        # 消息背景
        msg = game.message
        tw = draw.textlength(msg) if hasattr(draw, 'textlength') else len(msg)*8
        draw.text((W - tw - 12, bar_y), msg, fill=(255, 255, 200))

    if not game.alive:
        # 死亡覆盖
        overlay = Image.new("RGBA", (W, H), (0, 0, 0, 120))
        img = img.convert("RGBA")
        img = Image.alpha_composite(img, overlay)
        d2 = ImageDraw.Draw(img)
        # 大字
        if "通关" in game.message:
            d2.text((W//2-80, H//2-20), "🏆 通 关 !", fill=(255, 215, 0))
        else:
            d2.text((W//2-80, H//2-20), "💀 GAME OVER", fill=DANGER)
        d2.text((W//2-75, H//2+15), f"得分: {game.score}", fill=TEXT_C)
        img = img.convert("RGB")

    path = os.path.join(out_dir, f"frame_{frame_idx:05d}.png")
    img.save(path)
    return path

# ── 主程序 ──────────────────────────────────────────────
def main():
    import shutil
    frames_dir = "snake_frames"
    if os.path.exists(frames_dir):
        shutil.rmtree(frames_dir)
    os.makedirs(frames_dir)

    print("🎨 正在生成 AI 贪吃蛇动画帧...")
    game = SnakeGame()
    frame_paths = []
    max_frames = 300  # 最多 300 帧

    # 先渲染初始帧
    frame_paths.append(render_frame(game, 0, frames_dir))

    while game.alive and len(frame_paths) < max_frames:
        direction = ai_decide(game)
        game.step(direction)
        frame_paths.append(render_frame(game, len(frame_paths), frames_dir))

    # 死亡后多停留几帧
    for i in range(10):
        game.steps += 1
        frame_paths.append(render_frame(game, len(frame_paths), frames_dir))

    print(f"✅ 共生成 {len(frame_paths)} 帧 PNG")

    # 合成 GIF
    print("🎬 正在合成 GIF...")
    gif_path = "snake_ai_game.gif"
    # 用 ffmpeg
    import subprocess
    cmd = [
        "ffmpeg", "-y",
        "-framerate", "15",
        "-i", os.path.join(frames_dir, "frame_%05d.png"),
        "-vf", "fps=15,split[v1][v2];[v1]palettegen[p];[v2][p]paletteuse",
        gif_path
    ]
    subprocess.run(cmd, capture_output=True)

    # 清理帧文件
    shutil.rmtree(frames_dir)

    size = os.path.getsize(gif_path)
    print(f"🎉 完成！GIF: {gif_path} ({size/1024:.0f} KB)")
    print(f"   🍎 最终得分: {game.score}  |  🐍 蛇长: {len(game.snake)}  |  步数: {game.steps}")
    print(f"   {game.message}")

if __name__ == '__main__':
    main()
