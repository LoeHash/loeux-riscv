/*
 * plotter.c - 终端函数绘图器
 *
 * 编译（宿主机测试）:  gcc plotter.c -o plotter -lm
 * 编译（loeux 用户态）: 由 user/Makefile 自动交叉编译并装入镜像（加 -DLOEUX）
 *
 * 表达式语法: y = f(x)
 *   运算符: + - * / ^  一元 +/-  括号
 *   隐式乘法: 2x, 2(x+1), kx, 3sinx, 2x^2
 *   函数: sin cos tan asin acos atan sqrt exp log ln abs floor ceil
 *   变量: x, k, a, b（默认 1），其他单字母变量默认 1
 */

#ifdef LOEUX
#include <stdio.h>
#include <ulib.h>
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#endif

/* ======================== 字符分类（两模式通用） ======================== */
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* ======================== 数学函数 ======================== */
/*
 * loeux 用户态是 -nostdlib -ffreestanding，没有 libm，
 * 因此 sin/cos/pow 等全部自实现（static，不会与 -lm 冲突）。
 * 宿主机模式直接用 <math.h>。
 */
#ifdef LOEUX

#define M_PI  3.14159265358979323846
#define M_LN2 0.69314718055994530942

static double fabs(double x) { return x < 0 ? -x : x; }

static double floor(double x)
{
        long long i = (long long)x;
        if ((double)i > x)
                i--;
        return (double)i;
}

static double ceil(double x)
{
        long long i = (long long)x;
        if ((double)i < x)
                i++;
        return (double)i;
}

static double sqrt(double x)
{
        if (x <= 0)
                return 0;
        double r = x;
        for (int i = 0; i < 50; i++) {
                double nr = (r + x / r) * 0.5;
                if (nr == r)
                        break;
                r = nr;
        }
        return r;
}

static double sin(double x)
{
        /* 归约到 [-pi, pi] 再泰勒展开 */
        x = x - 2 * M_PI * floor(x / (2 * M_PI) + 0.5);
        double x2 = x * x;
        double term = x;
        double sum = x;
        for (int n = 1; n < 30; n++) {
                term *= -x2 / ((2.0 * n) * (2.0 * n + 1));
                sum += term;
        }
        return sum;
}

static double cos(double x) { return sin(x + M_PI / 2); }
static double tan(double x) { return sin(x) / cos(x); }

static double atan(double x)
{
        if (x < 0)
                return -atan(-x);
        /* 反复用 atan(x)=2*atan(x/(1+sqrt(1+x^2))) 把 x 缩到 0.2 以下 */
        int half = 0;
        while (x > 0.2) {
                x = x / (1 + sqrt(1 + x * x));
                half++;
        }
        double x2 = x * x;
        double term = x;
        double sum = x;
        for (int n = 1; n < 30; n++) {
                term *= -x2;
                sum += term / (2 * n + 1);
        }
        /* 还原：每缩一次要乘 2 */
        for (int i = 0; i < half; i++)
                sum *= 2;
        return sum;
}

static double asin(double x)
{
        if (x <= -1)
                return -M_PI / 2;
        if (x >= 1)
                return M_PI / 2;
        return atan(x / sqrt(1 - x * x));
}

static double acos(double x) { return M_PI / 2 - asin(x); }

static double exp(double x)
{
        if (x < 0)
                return 1.0 / exp(-x);
        /* x = k*ln2 + r，exp(x) = 2^k * exp(r)，r∈[0,ln2) 收敛快 */
        int k = (int)(x / M_LN2);
        double r = x - k * M_LN2;
        double term = 1, sum = 1;
        for (int n = 1; n < 30; n++) {
                term *= r / n;
                sum += term;
        }
        /* 2^k 快速幂 */
        double pow2k = 1, base = 2;
        int e = k;
        if (e < 0) {
                base = 0.5;
                e = -e;
        }
        while (e > 0) {
                if (e & 1)
                        pow2k *= base;
                base *= base;
                e >>= 1;
        }
        return sum * pow2k;
}

static double log(double x)
{
        if (x <= 0)
                return -1e308;
        /* 归一化 x = m * 2^k, m∈[1,2) */
        int k = 0;
        double m = x;
        while (m >= 2) {
                m /= 2;
                k++;
        }
        while (m < 1) {
                m *= 2;
                k--;
        }
        /* log(m) = 2*atanh((m-1)/(m+1))，z∈[0,1/3) 收敛快 */
        double z = (m - 1) / (m + 1);
        double z2 = z * z;
        double term = z, sum = z;
        for (int n = 1; n < 40; n++) {
                term *= z2;
                sum += term / (2 * n + 1);
        }
        return 2 * sum + k * M_LN2;
}

static double pow(double x, double y)
{
        if (y == 0)
                return 1;
        if (x == 0)
                return 0;
        /* 整数指数用快速幂，避免 exp/log 精度损失 */
        long long n = (long long)y;
        if ((double)n == y) {
                double result = 1, base = x;
                if (n < 0) {
                        base = 1 / x;
                        n = -n;
                }
                while (n > 0) {
                        if (n & 1)
                                result *= base;
                        base *= base;
                        n >>= 1;
                }
                return result;
        }
        if (x < 0)
                return 0; /* 负数的非整数次幂不处理 */
        return exp(y * log(x));
}

#endif /* LOEUX */

/* abs 对 double 取绝对值（两模式通用，避免依赖 fabs 命名差异） */
static double dabs(double x) { return x < 0 ? -x : x; }

/* ======================== 简单 double 解析（命令行参数用） ======================== */
static double parse_double(const char *s)
{
        double sign = 1;
        if (*s == '-') {
                sign = -1;
                s++;
        } else if (*s == '+') {
                s++;
        }
        double val = 0;
        while (is_digit(*s)) {
                val = val * 10 + (*s - '0');
                s++;
        }
        if (*s == '.') {
                s++;
                double frac = 0.1;
                while (is_digit(*s)) {
                        val += (*s - '0') * frac;
                        frac *= 0.1;
                        s++;
                }
        }
        return sign * val;
}

/* ======================== 表达式 AST ======================== */
typedef enum { N_NUM, N_VAR, N_BINOP, N_UNARY, N_FUNC } NType;

typedef struct Node {
        NType type;
        union {
                double num;
                char var;
                struct {
                        char op;
                        struct Node *l, *r;
                } bin;
                struct {
                        char op;
                        struct Node *operand;
                } un;
                struct {
                        const char *name;
                        struct Node *arg;
                } fn;
        } u;
} Node;

/* 固定 arena，避免依赖 malloc */
#define ARENA_SIZE 512
static Node node_arena[ARENA_SIZE];
static int arena_pos = 0;

static Node *new_node(NType t)
{
        Node *n = &node_arena[arena_pos++];
        n->type = t;
        return n;
}

/* ======================== 递归下降解析器 ======================== */
typedef struct {
        const char *s;
        int pos;
} Parser;

static void skip_ws(Parser *p)
{
        while (is_space(p->s[p->pos]))
                p->pos++;
}

static char peek(Parser *p)
{
        skip_ws(p);
        return p->s[p->pos];
}

static char getch(Parser *p)
{
        skip_ws(p);
        return p->s[p->pos++];
}

/*
 * 字母串匹配：先尝试已知函数名（按长度降序），
 * 不是函数才当作单字母变量。
 * 这样 kx 不会被误判成函数名返回 0，而是 k 与 x 两个变量。
 */
static const char *func_names[] = {"floor", "asin", "acos", "atan",
                                   "sqrt",  "ceil", "sin",  "cos",
                                   "tan",   "exp",  "log",  "abs",
                                   "ln"};
static const int func_count = sizeof(func_names) / sizeof(func_names[0]);

static const char *match_function(Parser *p)
{
        for (int i = 0; i < func_count; i++) {
                const char *fn = func_names[i];
                int len = (int)strlen(fn);
                if (strncmp(p->s + p->pos, fn, len) == 0) {
                        p->pos += len;
                        return fn;
                }
        }
        return NULL;
}

static Node *parse_expr(Parser *p);
static Node *parse_term(Parser *p);
static Node *parse_factor(Parser *p);
static Node *parse_power(Parser *p);
static Node *parse_unary(Parser *p);
static Node *parse_primary(Parser *p);

/* factor 层处理隐式乘法：幂运算之后、乘除之前。
 * 连续的 power（数字、变量、括号、函数）之间自动相乘，
 * 保证 2x^2 = 2*(x^2)、sin(2x+2) 中 2x = 2*x。 */
static Node *parse_factor(Parser *p)
{
        Node *left = parse_power(p);
        while (1) {
                char c = peek(p);
                if (is_digit(c) || c == '.' || c == '(' || is_alpha(c)) {
                        Node *right = parse_power(p);
                        Node *n = new_node(N_BINOP);
                        n->u.bin.op = '*';
                        n->u.bin.l = left;
                        n->u.bin.r = right;
                        left = n;
                } else {
                        break;
                }
        }
        return left;
}

static Node *parse_term(Parser *p)
{
        Node *left = parse_factor(p);
        while (1) {
                char c = peek(p);
                if (c == '*' || c == '/') {
                        getch(p);
                        Node *right = parse_factor(p);
                        Node *n = new_node(N_BINOP);
                        n->u.bin.op = c;
                        n->u.bin.l = left;
                        n->u.bin.r = right;
                        left = n;
                } else {
                        break;
                }
        }
        return left;
}

static Node *parse_expr(Parser *p)
{
        Node *left = parse_term(p);
        while (1) {
                char c = peek(p);
                if (c == '+' || c == '-') {
                        getch(p);
                        Node *right = parse_term(p);
                        Node *n = new_node(N_BINOP);
                        n->u.bin.op = c;
                        n->u.bin.l = left;
                        n->u.bin.r = right;
                        left = n;
                } else {
                        break;
                }
        }
        return left;
}

/* power 右结合：2^3^2 = 2^(3^2) */
static Node *parse_power(Parser *p)
{
        Node *base = parse_unary(p);
        if (peek(p) == '^') {
                getch(p);
                Node *exp = parse_factor(p); /* 右侧取完整 factor 实现右结合 */
                Node *n = new_node(N_BINOP);
                n->u.bin.op = '^';
                n->u.bin.l = base;
                n->u.bin.r = exp;
                return n;
        }
        return base;
}

static Node *parse_unary(Parser *p)
{
        char c = peek(p);
        if (c == '+' || c == '-') {
                getch(p);
                Node *operand = parse_unary(p);
                Node *n = new_node(N_UNARY);
                n->u.un.op = c;
                n->u.un.operand = operand;
                return n;
        }
        return parse_primary(p);
}

static double apply_func(const char *name, double arg)
{
        if (strcmp(name, "sin") == 0)
                return sin(arg);
        if (strcmp(name, "cos") == 0)
                return cos(arg);
        if (strcmp(name, "tan") == 0)
                return tan(arg);
        if (strcmp(name, "asin") == 0)
                return asin(arg);
        if (strcmp(name, "acos") == 0)
                return acos(arg);
        if (strcmp(name, "atan") == 0)
                return atan(arg);
        if (strcmp(name, "sqrt") == 0)
                return sqrt(arg);
        if (strcmp(name, "exp") == 0)
                return exp(arg);
        if (strcmp(name, "log") == 0)
                return log(arg);
        if (strcmp(name, "ln") == 0)
                return log(arg); /* ln 与 log 都是自然对数 */
        if (strcmp(name, "abs") == 0)
                return dabs(arg);
        if (strcmp(name, "floor") == 0)
                return floor(arg);
        if (strcmp(name, "ceil") == 0)
                return ceil(arg);
        return 0;
}

static Node *parse_primary(Parser *p)
{
        char c = peek(p);

        /* 数字 */
        if (is_digit(c) || c == '.') {
                /* 直接用 parse_double 在当前位置解析 */
                const char *start = p->s + p->pos;
                char buf[64];
                int i = 0;
                while (i < 63 && (is_digit(p->s[p->pos]) || p->s[p->pos] == '.')) {
                        buf[i++] = p->s[p->pos++];
                }
                buf[i] = '\0';
                (void)start;
                Node *n = new_node(N_NUM);
                n->u.num = parse_double(buf);
                return n;
        }

        /* 括号 */
        if (c == '(') {
                getch(p);
                Node *e = parse_expr(p);
                if (peek(p) != ')') {
                        printf("parse error: expected ')'\n");
                        return NULL;
                }
                getch(p);
                return e;
        }

        /* 字母：先试函数名，再试变量 */
        if (is_alpha(c)) {
                const char *fname = match_function(p);
                if (fname) {
                        Node *n = new_node(N_FUNC);
                        n->u.fn.name = fname;
                        skip_ws(p);
                        if (peek(p) == '(') {
                                getch(p);
                                n->u.fn.arg = parse_expr(p);
                                if (peek(p) != ')') {
                                        printf("parse error: expected ')'\n");
                                        return NULL;
                                }
                                getch(p);
                        } else {
                                /* 无括号：作用于后面的一元/基本量，如 sinx -> sin(x) */
                                n->u.fn.arg = parse_unary(p);
                        }
                        return n;
                }
                /* 单字母变量 */
                char var = getch(p);
                Node *n = new_node(N_VAR);
                n->u.var = var;
                return n;
        }

        printf("parse error: unexpected char '%c'\n", c);
        return NULL;
}

static Node *parse(const char *expr)
{
        Parser p;
        p.s = expr;
        p.pos = 0;
        arena_pos = 0;
        Node *n = parse_expr(&p);
        if (peek(&p) != '\0') {
                printf("parse error: trailing chars at '%s'\n", p.s + p.pos);
                return NULL;
        }
        return n;
}

/* ======================== 求值 ======================== */
static double eval(Node *n, double x, double k, double a, double b)
{
        if (!n)
                return 0;
        switch (n->type) {
        case N_NUM:
                return n->u.num;
        case N_VAR:
                if (n->u.var == 'x')
                        return x;
                if (n->u.var == 'k')
                        return k;
                if (n->u.var == 'a')
                        return a;
                if (n->u.var == 'b')
                        return b;
                return 1.0; /* 未知单字母变量默认 1 */
        case N_BINOP: {
                double l = eval(n->u.bin.l, x, k, a, b);
                double r = eval(n->u.bin.r, x, k, a, b);
                switch (n->u.bin.op) {
                case '+':
                        return l + r;
                case '-':
                        return l - r;
                case '*':
                        return l * r;
                case '/':
                        return l / r;
                case '^':
                        return pow(l, r);
                }
        }
        case N_UNARY:
                return n->u.un.op == '-' ? -eval(n->u.un.operand, x, k, a, b)
                                          : eval(n->u.un.operand, x, k, a, b);
        case N_FUNC:
                return apply_func(n->u.fn.name, eval(n->u.fn.arg, x, k, a, b));
        }
        return 0;
}

/* ======================== 绘图 ======================== */
#define COLS 80
#define ROWS 24
#define NUM_SAMPLES 16 /* 带 range 的变量采样数 */

static char grid[ROWS][COLS + 1];

static void clear_grid(void)
{
        for (int r = 0; r < ROWS; r++) {
                for (int c = 0; c < COLS; c++)
                        grid[r][c] = ' ';
                grid[r][COLS] = '\0';
        }
}

static void draw_axes(double xmin, double xmax, double ymin, double ymax)
{
        int row_y0 = -1, col_x0 = -1;
        if (ymin <= 0 && ymax >= 0) {
                row_y0 = (int)((ymax - 0) / (ymax - ymin) * (ROWS - 1) + 0.5);
                if (row_y0 < 0)
                        row_y0 = 0;
                if (row_y0 >= ROWS)
                        row_y0 = ROWS - 1;
                for (int c = 0; c < COLS; c++)
                        grid[row_y0][c] = '-';
        }
        if (xmin <= 0 && xmax >= 0) {
                col_x0 = (int)((0 - xmin) / (xmax - xmin) * (COLS - 1) + 0.5);
                if (col_x0 < 0)
                        col_x0 = 0;
                if (col_x0 >= COLS)
                        col_x0 = COLS - 1;
                for (int r = 0; r < ROWS; r++)
                        grid[r][col_x0] = '|';
        }
        if (row_y0 >= 0 && col_x0 >= 0)
                grid[row_y0][col_x0] = '+';
}

static void plot_point(double x, double y, double xmin, double xmax,
                       double ymin, double ymax)
{
        int col = (int)((x - xmin) / (xmax - xmin) * (COLS - 1) + 0.5);
        int row = (int)((ymax - y) / (ymax - ymin) * (ROWS - 1) + 0.5);
        if (col >= 0 && col < COLS && row >= 0 && row < ROWS)
                grid[row][col] = '*';
}

/* 颜色名 -> ANSI 码 */
static const char *color_code(const char *name)
{
        if (strcmp(name, "red") == 0)
                return "\033[31m";
        if (strcmp(name, "green") == 0)
                return "\033[32m";
        if (strcmp(name, "yellow") == 0)
                return "\033[33m";
        if (strcmp(name, "blue") == 0)
                return "\033[34m";
        if (strcmp(name, "magenta") == 0)
                return "\033[35m";
        if (strcmp(name, "cyan") == 0)
                return "\033[36m";
        if (strcmp(name, "white") == 0)
                return "\033[37m";
        if (strcmp(name, "bright_red") == 0)
                return "\033[91m";
        if (strcmp(name, "bright_green") == 0)
                return "\033[92m";
        if (strcmp(name, "bright_yellow") == 0)
                return "\033[93m";
        if (strcmp(name, "bright_blue") == 0)
                return "\033[94m";
        if (strcmp(name, "bright_magenta") == 0)
                return "\033[95m";
        if (strcmp(name, "bright_cyan") == 0)
                return "\033[96m";
        return NULL;
}

/* 把 double 格式化为字符串（loeux 的 printf 不支持 %f） */
static void fmt_double(double v, char *buf, int prec)
{
        char *p = buf;
        if (v < 0) {
                *p++ = '-';
                v = -v;
        }
        long long ipart = (long long)v;
        double frac = v - (double)ipart;
        char tmp[32];
        int n = 0;
        if (ipart == 0)
                tmp[n++] = '0';
        while (ipart > 0) {
                tmp[n++] = '0' + (int)(ipart % 10);
                ipart /= 10;
        }
        while (n > 0)
                *p++ = tmp[--n];
        if (prec > 0) {
                *p++ = '.';
                for (int i = 0; i < prec; i++) {
                        frac *= 10;
                        int d = (int)frac;
                        *p++ = '0' + d;
                        frac -= d;
                }
        }
        *p = '\0';
}

/* ======================== range 解析 ======================== */
/*
 * 易错点：解析 "1-100" 时，第一个 strtod/parse_double 读到 lo 后，
 * 必须显式跳过分隔符 '-'，再从 '-' 之后读 hi。
 * 否则像 "-5-5" 会把第二个 '-' 当成负号，得到 hi=-5，
 * 导致 range 反向、y 窗口退化成 [-1,1]、曲线变横线。
 * 支持 "1-100"、"-5-5"、"-3--1" 三种写法。
 */
static int parse_range(const char *s, double *lo, double *hi)
{
        const char *p = s;
        *lo = parse_double(p);
        /* 跳过 lo 之后的字符（数字和小数点） */
        if (*p == '-' || *p == '+')
                p++;
        while (is_digit(*p) || *p == '.')
                p++;
        while (is_space(*p))
                p++;
        /* 显式跳过分隔符 '-' */
        if (*p != '-')
                return -1;
        p++;
        while (is_space(*p))
                p++;
        *hi = parse_double(p);
        return 0;
}

/* ======================== 帮助 ======================== */
static void print_help(void)
{
        printf("Usage: plotter -f <expr> [options]\n");
        printf("  -f <expr>       function expression y = f(x)\n");
        printf("  -k <val>        set parameter k (default 1)\n");
        printf("  -a <val>        set parameter a (default 1)\n");
        printf("  -b <val>        set parameter b (default 1)\n");
        printf("  -c <color>      line color (default green)\n");
        printf("                  colors: red green yellow blue magenta cyan white\n");
        printf("                          bright_red bright_green bright_yellow\n");
        printf("                          bright_blue bright_magenta bright_cyan\n");
        printf("  -range-<var> lo-hi   sample <var> in [lo,hi], overlay curves\n");
        printf("                  e.g. -range-k 1-100, -range-x 0-20, -range-a -5-5\n");
        printf("  -h, --help      show this help\n");
        printf("\nExamples:\n");
        printf("  plotter -f 'sinx'\n");
        printf("  plotter -f 'sin(x) + 1' -c red\n");
        printf("  plotter -f 'sin(2x+2)' -c yellow\n");
        printf("  plotter -f 'kx' -k 2 -c blue\n");
        printf("  plotter -f 'kx' -range-k 1-100 -c cyan\n");
        printf("  plotter -f 'sin(kx)' -range-k 1-5 -c bright_magenta\n");
        printf("  plotter -f 'sinx' -range-x 0-20 -c yellow\n");
}

/* ======================== 主函数 ======================== */
int main(int argc, char *argv[])
{
        const char *expr = NULL;
        const char *color_name = "green";
        double k_val = 1, a_val = 1, b_val = 1;

        /* range 变量 */
        char range_var = 0;       /* 0 表示无 range 变量（x 除外） */
        double range_lo = 0, range_hi = 0;
        int has_range = 0;

        /* x 显示范围 */
        double xmin = -10, xmax = 10;
        int has_x_range = 0;

        for (int i = 1; i < argc; i++) {
                const char *arg = argv[i];

                if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
                        print_help();
                        return 0;
                }
                if (strcmp(arg, "-f") == 0) {
                        if (i + 1 >= argc) {
                                printf("error: -f needs an expression\n");
                                return 1;
                        }
                        expr = argv[++i];
                        continue;
                }
                if (strcmp(arg, "-k") == 0 || strcmp(arg, "-a") == 0 ||
                    strcmp(arg, "-b") == 0) {
                        if (i + 1 >= argc) {
                                printf("error: %s needs a value\n", arg);
                                return 1;
                        }
                        double v = parse_double(argv[++i]);
                        if (arg[1] == 'k')
                                k_val = v;
                        else if (arg[1] == 'a')
                                a_val = v;
                        else
                                b_val = v;
                        continue;
                }
                if (strcmp(arg, "-c") == 0) {
                        if (i + 1 >= argc) {
                                printf("error: -c needs a color\n");
                                return 1;
                        }
                        color_name = argv[++i];
                        continue;
                }
                /* -range-<var> lo-hi */
                if (strncmp(arg, "-range-", 7) == 0) {
                        const char *varname = arg + 7;
                        if (varname[0] == '\0' || varname[1] != '\0') {
                                printf("error: -range-<var> needs a single-letter variable\n");
                                return 1;
                        }
                        if (i + 1 >= argc) {
                                printf("error: -range-%s needs lo-hi\n", varname);
                                return 1;
                        }
                        double lo, hi;
                        if (parse_range(argv[++i], &lo, &hi) != 0) {
                                printf("error: invalid range '%s'\n", argv[i]);
                                return 1;
                        }
                        if (varname[0] == 'x') {
                                xmin = lo;
                                xmax = hi;
                                has_x_range = 1;
                        } else {
                                range_var = varname[0];
                                range_lo = lo;
                                range_hi = hi;
                                has_range = 1;
                        }
                        continue;
                }
                printf("error: unknown argument '%s'\n", arg);
                return 1;
        }

        if (!expr) {
                printf("error: no expression given. Use -f <expr> or -h for help.\n");
                return 1;
        }

        const char *ccode = color_code(color_name);
        if (!ccode) {
                printf("error: unknown color '%s'\n", color_name);
                return 1;
        }

        Node *ast = parse(expr);
        if (!ast) {
                printf("error: failed to parse expression\n");
                return 1;
        }

        /* 打印标题 */
        printf("y = %s\n", expr);
        if (has_range) {
                char lo_s[32], hi_s[32];
                fmt_double(range_lo, lo_s, 2);
                fmt_double(range_hi, hi_s, 2);
                printf("%c in [%s, %s], %d curves\n", range_var, lo_s, hi_s,
                       NUM_SAMPLES);
        }

        /* 第一遍：扫描所有曲线的 y 最小/最大值，自适应纵向缩放 */
        double ymin = 1e308, ymax = -1e308;
        for (int s = 0; s < (has_range ? NUM_SAMPLES : 1); s++) {
                double k = k_val, a = a_val, b = b_val;
                if (has_range) {
                        double t = (NUM_SAMPLES == 1)
                                       ? 0
                                       : (double)s / (NUM_SAMPLES - 1);
                        double v = range_lo + t * (range_hi - range_lo);
                        if (range_var == 'k')
                                k = v;
                        else if (range_var == 'a')
                                a = v;
                        else if (range_var == 'b')
                                b = v;
                }
                for (int c = 0; c < COLS; c++) {
                        double x = xmin + (double)c / (COLS - 1) * (xmax - xmin);
                        double y = eval(ast, x, k, a, b);
                        if (y < ymin)
                                ymin = y;
                        if (y > ymax)
                                ymax = y;
                }
        }

        /* 退化情况：常数函数 ymin==ymax */
        if (ymin == ymax) {
                ymin -= 1;
                ymax += 1;
        } else {
                /* 留一点边距 */
                double pad = (ymax - ymin) * 0.05;
                ymin -= pad;
                ymax += pad;
        }

        /* 第二遍：绘图 */
        clear_grid();
        draw_axes(xmin, xmax, ymin, ymax);

        for (int s = 0; s < (has_range ? NUM_SAMPLES : 1); s++) {
                double k = k_val, a = a_val, b = b_val;
                if (has_range) {
                        double t = (NUM_SAMPLES == 1)
                                       ? 0
                                       : (double)s / (NUM_SAMPLES - 1);
                        double v = range_lo + t * (range_hi - range_lo);
                        if (range_var == 'k')
                                k = v;
                        else if (range_var == 'a')
                                a = v;
                        else if (range_var == 'b')
                                b = v;
                }
                for (int c = 0; c < COLS; c++) {
                        double x = xmin + (double)c / (COLS - 1) * (xmax - xmin);
                        double y = eval(ast, x, k, a, b);
                        plot_point(x, y, xmin, xmax, ymin, ymax);
                }
        }

        /* 输出（带颜色） */
        printf("%s", ccode);
        for (int r = 0; r < ROWS; r++)
                printf("%s\n", grid[r]);
        printf("\033[0m");

        /* 打印 x/y 范围 */
        char xs1[32], xs2[32], ys1[32], ys2[32];
        fmt_double(xmin, xs1, 2);
        fmt_double(xmax, xs2, 2);
        fmt_double(ymin, ys1, 2);
        fmt_double(ymax, ys2, 2);
        printf("x: [%s, %s]  y: [%s, %s]\n", xs1, xs2, ys1, ys2);

        return 0;
}
