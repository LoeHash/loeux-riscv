#include <ulib.h>
#include <stdio.h>

#define LSH_MAX_LINE 1024 // 命令行最大长度
#define LSH_MAX_ARGS 64   // 最大参数个数

int lsh_cd(char **args);
int lsh_help(char **args);
int lsh_exit(char **args);

char *builtin_str[] = {"cd", "help", "exit"};
int (*builtin_func[])(char **) = {&lsh_cd, &lsh_help, &lsh_exit};

int lsh_num_builtins()
{
        return sizeof(builtin_str) / sizeof(char *);
}

// ========== 内建命令实现 ==========
int lsh_cd(char **args)
{
        if (args[1] == NULL)
        {
                printf("lsh: expected argument to \"cd\"\n");
        }
        else
        {
                if (chdir(args[1]) != 0)
                {
                        printf("lsh: cd failed\n");
                }
        }
        return 1;
}

int lsh_help(char **args)
{
        int i;
        printf("Stephen Brennan's LSH\n");
        printf("Type program names and arguments, and hit enter.\n");
        printf("The following are built in:\n");
        for (i = 0; i < lsh_num_builtins(); i++)
        {
                printf("  %s\n", builtin_str[i]);
        }
        printf("Use the man command for information on other programs.\n");
        return 1;
}

int lsh_exit(char **args)
{
        return 0;
}

// ========== 程序启动（fork + exec）==========
int lsh_launch(char **args)
{
        pid_t pid;
        int status;

        pid = fork();
        if (pid == 0)
        {
                // 子进程
                if (exec(args[0], args) == -1)
                {
                        printf("lsh: command \"%s\" not found\n", args[0]);
                }
                exit(1);
        }
        else if (pid < 0)
        {
                printf("lsh: fork failed\n");
        }
        else
        {
                // 父进程等待指定子进程退出
                waitpid(pid, &status);
        }
        return 1;
}

// ========== 命令执行 ==========
int lsh_execute(char **args)
{
        int i;

        if (args[0] == NULL)
        {
                return 1;
        }

        for (i = 0; i < lsh_num_builtins(); i++)
        {
                if (strcmp(args[0], builtin_str[i]) == 0)
                {
                        return (*builtin_func[i])(args);
                }
        }

        return lsh_launch(args);
}

char *lsh_read_line(void)
{
        static char buffer[LSH_MAX_LINE];
        int position = 0;
        int c;

        while (1)
        {
                c = getchar();
                if (c == EOF)
                {
                        exit(EXIT_SUCCESS);
                }
                else if (c == '\n')
                {
                        buffer[position] = '\0';
                        return buffer;
                }
                else if (position < LSH_MAX_LINE - 1)
                {
                        buffer[position++] = c;
                }
                else
                {
                        // 命令太长，截断
                        buffer[position] = '\0';
                        printf("lsh: command too long\n");
                        while ((c = getchar()) != '\n' && c != EOF)
                                ;
                        return buffer;
                }
        }
}

char **lsh_split_line(char *line)
{
        static char *args[LSH_MAX_ARGS];
        int position = 0;
        char *token;

        token = strtok(line, " \t\r\n\a");
        while (token != NULL && position < LSH_MAX_ARGS - 1)
        {
                args[position++] = token;
                token = strtok(NULL, " \t\r\n\a");
        }
        args[position] = NULL;
        return args;
}

void lsh_loop(void)
{
        char *line;
        char **args;
        int status;

        do
        {
                printf("> ");
                line = lsh_read_line();
                args = lsh_split_line(line);
                status = lsh_execute(args);
        } while (status);
}

int main(int argc, char **argv)
{
        lsh_loop();
        return 0;
}