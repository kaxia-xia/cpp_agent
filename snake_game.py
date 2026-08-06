#!/usr/bin/env python3
"""🐍 AI 贪吃蛇 — 咱自己玩给你看！"""

import sys
import os
import time
import random
from collections import deque

# ── 终端控制 ────────────────────────────────────────────
def hide_cursor():
    sys.stdout.write("\033[?25l")
    sys.stdout.flush()

def show_cursor():
    sys.stdout.write("\033[?25h")
    sys.stdout.flush()

def clear_screen():
    sys.stdout.write("\033[2J\033[H")
    sys.stdout.flush()

# ── 游戏配置 ────────────────────────────────────────────
W, H = 40, 20  # 地图宽高

# 方向向量: (dx, dy)
DIRS = {
    'U': (0, -1), 'D': (0, 1), 'L': (-1, 0), 'R': (1, 0)
}
REVERSE = {'U': 'D', 'D': 'U', 'L': 'R', 'R': 'L'}

# ── 颜文字 & 颜色 ───────────────────────────────────────
COLORS = {
    'head':   '\033[1;32m',   # 亮绿蛇头
    'body':   '\033[0;32m',   # 绿蛇身
    'food':   '\033[1;35m',   # 紫红色食物
    'border': '\033[0;34m',   # 蓝边框
    'score':  '\033[1;33m',   # 金色分数
    'reset':  '\033[0m',
}

SNAKE_FACE = {
    'U': '▲', 'D': '▼', 'L': '◀', 'R': '▶'
}

# ── 游戏状态 ────────────────────────────────────────────
class SnakeGame:
    def __init__(self):
        self.snake = deque()
        # 初始蛇：水平方向，朝右
        cx, cy = W // 2, H // 2
        for i in range(4):
            self.snake.append((cx - i, cy))
        self.dir = 'R'
        self.next_dir = 'R'
        self.food = None
        self.score = 0
        self.steps = 0
        self.alive = True
        self.spawn_food()

    def spawn_food(self):
        occupied = set(self.snake)
        empty = [(x, y) for x in range(1, W-1) for y in range(1, H-1)
                 if (x, y) not in occupied]
        if empty:
            self.food = random.choice(empty)
        else:
            self.food = None  # 赢了！

    def step(self, direction):
        """执行一步，返回 (alive, ate_food)"""
        self.steps += 1
        self.dir = direction

        head_x, head_y = self.snake[0]
        dx, dy = DIRS[direction]
        nx, ny = head_x + dx, head_y + dy

        # 撞墙检测
        if nx <= 0 or nx >= W-1 or ny <= 0 or ny >= H-1:
            self.alive = False
            return False, False

        # 撞自己检测（尾巴尖即将移走，不算撞）
        body = set(self.snake)
        # 如果吃到食物，尾巴不会缩
        ate = (nx, ny) == self.food
        if not ate:
            body.discard(self.snake[-1])  # 尾巴会移走

        if (nx, ny) in body:
            self.alive = False
            return False, False

        # 移动
        self.snake.appendleft((nx, ny))
        if ate:
            self.score += 1
            self.spawn_food()
            if self.food is None:
                self.alive = False  # 赢了！
        else:
            self.snake.pop()

        return True, ate

    def get_grid(self):
        """生成当前游戏画面的字符网格"""
        grid = [[' ' for _ in range(W)] for _ in range(H)]

        # 边框
        for x in range(W):
            grid[0][x] = '▓'
            grid[H-1][x] = '▓'
        for y in range(H):
            grid[y][0] = '▓'
            grid[y][W-1] = '▓'

        # 蛇身
        for i, (sx, sy) in enumerate(self.snake):
            if 0 <= sx < W and 0 <= sy < H:
                grid[sy][sx] = 'o' if i > 0 else '●'

        # 食物
        if self.food:
            fx, fy = self.food
            grid[fy][fx] = '♥'

        return grid

# ── AI 大脑 ─────────────────────────────────────────────
def bfs_path(start, target, snake_set, w, h):
    """BFS 找最短路径，返回下一步方向（或 None 表示无路可走）"""
    if start == target:
        return None

    visited = set()
    queue = deque()
    # (x, y, first_step_dir)
    for d, (dx, dy) in DIRS.items():
        nx, ny = start[0] + dx, start[1] + dy
        if (nx, ny) not in snake_set and 0 < nx < w-1 and 0 < ny < h-1:
            queue.append((nx, ny, d))
            visited.add((nx, ny))

    while queue:
        cx, cy, first_dir = queue.popleft()
        if (cx, cy) == target:
            return first_dir
        for d, (dx, dy) in DIRS.items():
            nx, ny = cx + dx, cy + dy
            if (nx, ny) not in visited and (nx, ny) not in snake_set \
               and 0 < nx < w-1 and 0 < ny < h-1:
                visited.add((nx, ny))
                queue.append((nx, ny, first_dir))

    return None  # 无路可走

def ai_decide(game):
    """AI 决策：返回下一步方向"""
    head = game.snake[0]
    food = game.food
    if not food:
        return game.dir

    # 蛇身集合（不算尾部，因为尾部会移走）
    body_set = set(game.snake)
    body_set.discard(game.snake[-1])

    # 尝试找去食物的最短路径
    path_dir = bfs_path(head, food, body_set, W, H)

    if path_dir is not None:
        # 模拟走一步后是否还安全
        dx, dy = DIRS[path_dir]
        nx, ny = head[0] + dx, head[1] + dy

        # 模拟吃到食物后的新蛇身
        if (nx, ny) == food:
            new_snake = deque(game.snake)
            new_snake.appendleft((nx, ny))
            # 检查吃完后能不能到下一个食物（如果有的话暂时不知道，先检查空间）
            # 简单策略：如果蛇很长且接近满，小心点
            if len(new_snake) > W * H * 0.7:
                # 尝试跟尾巴走
                tail = game.snake[-1]
                follow_dir = bfs_path(head, tail, body_set, W, H)
                if follow_dir and follow_dir != REVERSE.get(game.dir):
                    return follow_dir
            return path_dir

        # 检查走过去后是否还有路
        future_body = set(game.snake)
        future_body.discard(game.snake[-1])
        future_body.add((nx, ny))
        # 从新位置到食物还有路吗？
        future_path = bfs_path((nx, ny), food, future_body, W, H)
        if future_path is not None:
            return path_dir

    # 去食物的路不通，尝试追尾巴（保命策略）
    tail = game.snake[-1]
    tail_dir = bfs_path(head, tail, body_set, W, H)
    if tail_dir and tail_dir != REVERSE.get(game.dir):
        return tail_dir

    # 随便走一个安全的方向
    safe_dirs = []
    for d, (dx, dy) in DIRS.items():
        if d == REVERSE.get(game.dir):
            continue
        nx, ny = head[0] + dx, head[1] + dy
        if (nx, ny) not in body_set and 0 < nx < W-1 and 0 < ny < H-1:
            safe_dirs.append(d)

    if safe_dirs:
        return random.choice(safe_dirs)

    return game.dir  # 无路可走，听天由命

# ── 渲染 ───────────────────────────────────────────────
def render(game, message=""):
    grid = game.get_grid()

    # 带颜色输出
    lines = []
    for y in range(H):
        line = ""
        for x in range(W):
            ch = grid[y][x]
            if ch == '●':
                face = SNAKE_FACE.get(game.dir, '●')
                line += f"{COLORS['head']}{face}{COLORS['reset']}"
            elif ch == 'o':
                line += f"{COLORS['body']}○{COLORS['reset']}"
            elif ch == '♥':
                # 食物闪烁效果
                pulse = '♥' if game.steps % 3 < 2 else '♡'
                line += f"{COLORS['food']}{pulse}{COLORS['reset']}"
            elif ch == '▓':
                line += f"{COLORS['border']}▓{COLORS['reset']}"
            else:
                line += ' '
        lines.append(line)

    # 信息栏
    info = (
        f"\n  {COLORS['score']}🍎 得分: {game.score:04d}"
        f"  |  蛇长: {len(game.snake):03d}"
        f"  |  步数: {game.steps:05d}{COLORS['reset']}"
        f"  |  {message}"
    )

    # 清屏重绘
    sys.stdout.write("\033[H")  # 光标回左上角
    sys.stdout.write("\n".join(lines))
    sys.stdout.write(info)
    sys.stdout.write("\n")
    sys.stdout.flush()

# ── 主循环 ──────────────────────────────────────────────
def main():
    os.system('')  # 启用 Windows 终端 ANSI（不影响 Linux）
    hide_cursor()
    clear_screen()

    print(f"\n  {COLORS['score']}🐍 AI 贪吃蛇 — 咱来玩给你看！{COLORS['reset']}\n")
    print("  正在初始化...\n")
    time.sleep(0.5)

    game = SnakeGame()
    speed = 0.06  # 初始速度（秒/步）

    messages = [
        "喵~ 咱来啦！",
        "嘿嘿，这颗也要吃掉~",
        "小心小心…",
        "好险！差点撞墙",
        "冲鸭！！",
        "嗯…让我想想🤔",
        "这颗♥是我的！",
        "啦啦啦~",
        "好长好长了！",
        "咱好厉害对不对！",
        "emmm...绕一下",
        "完美路线！",
        "吃吃吃！",
    ]

    try:
        while game.alive:
            # AI 决策
            direction = ai_decide(game)

            # 偶尔说说骚话
            msg = random.choice(messages) if random.random() < 0.15 else ""

            # 执行
            alive, ate = game.step(direction)
            if ate:
                msg = "啊呜~ 好吃！😋"

            render(game, msg)

            # 动态调速：蛇越长越快
            speed = max(0.02, 0.08 - len(game.snake) * 0.0008)
            time.sleep(speed)

        # 游戏结束
        if game.food is None and len(game.snake) == (W-2) * (H-2):
            end_msg = f"{COLORS['score']}🎉 天哪！！咱把整个地图填满了！！满分通关！！{COLORS['reset']}"
        elif game.food is None:
            end_msg = f"{COLORS['score']}🏆 赢了！食物吃光啦！得分: {game.score}{COLORS['reset']}"
        else:
            cause = "撞墙了😭" if any(
                game.snake[0][0] <= 0 or game.snake[0][0] >= W-1 or
                game.snake[0][1] <= 0 or game.snake[0][1] >= H-1
            ) else "咬到自己了😵"
            end_msg = f"\033[1;31m💀 游戏结束 — {cause}  最终得分: {game.score}\033[0m"

        render(game, end_msg)
        print(f"\n  {end_msg}\n")
        time.sleep(0.5)

    except KeyboardInterrupt:
        print(f"\n\n  {COLORS['score']}👋 被中断啦~ 得分: {game.score}{COLORS['reset']}\n")

    finally:
        show_cursor()
        print("\n  🐱 谢谢观看！咱玩得开心吗？\n")


if __name__ == '__main__':
    main()
