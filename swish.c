#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job_list.h"
#include "string_vector.h"
#include "swish_funcs.h"

#define CMD_LEN 512
#define PROMPT "@> "

int main(int argc, char **argv) {
    // Task 4: Set up shell to ignore SIGTTIN, SIGTTOU when put in background
    // You should adapt this code for use in run_command().
    struct sigaction sac;
    sac.sa_handler = SIG_IGN;
    if (sigfillset(&sac.sa_mask) == -1) {
        perror("sigfillset");
        return 1;
    }
    sac.sa_flags = 0;
    if (sigaction(SIGTTIN, &sac, NULL) == -1 || sigaction(SIGTTOU, &sac, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    strvec_t tokens;
    strvec_init(&tokens);
    job_list_t jobs;
    job_list_init(&jobs);
    char cmd[CMD_LEN];

    printf("%s", PROMPT);
    while (fgets(cmd, CMD_LEN, stdin) != NULL) {
        // Need to remove trailing '\n' from cmd. There are fancier ways.
        int i = 0;
        while (cmd[i] != '\n') {
            i++;
        }
        cmd[i] = '\0';

        if (tokenize(cmd, &tokens) != 0) {
            printf("Failed to parse command\n");
            strvec_clear(&tokens);
            job_list_free(&jobs);
            return 1;
        }
        if (tokens.length == 0) {
            printf("%s", PROMPT);
            continue;
        }
        const char *first_token = strvec_get(&tokens, 0);

        if (strcmp(first_token, "pwd") == 0) {
            char buf[CMD_LEN];
            if (getcwd(buf, CMD_LEN) == NULL) {
                perror("getcwd");
            } else {
                printf("%s\n", buf);
            }
        }

        else if (strcmp(first_token, "cd") == 0) {
            // argument for directory to change to
            const char *second_token = strvec_get(&tokens, 1);
            const char *dir;

            // if cd is used alone, move to user's home directory
            if (second_token == NULL) {
                dir = getenv("HOME");
            } else {
                dir = second_token;
            }

            // change the directory and handle errors accordingly
            if (chdir(dir) != 0) {
                perror("chdir");
            }
        }

        else if (strcmp(first_token, "exit") == 0) {
            strvec_clear(&tokens);
            break;
        }

        // Task 5: Print out current list of pending jobs
        else if (strcmp(first_token, "jobs") == 0) {
            int i = 0;
            job_t *current = jobs.head;
            while (current != NULL) {
                char *status_desc;
                if (current->status == BACKGROUND) {
                    status_desc = "background";
                } else {
                    status_desc = "stopped";
                }
                printf("%d: %s (%s)\n", i, current->name, status_desc);
                i++;
                current = current->next;
            }
        }

        // Task 5: Move stopped job into foreground
        else if (strcmp(first_token, "fg") == 0) {
            if (resume_job(&tokens, &jobs, 1) == -1) {
                printf("Failed to resume job in foreground\n");
            }
        }

        // Task 6: Move stopped job into background
        else if (strcmp(first_token, "bg") == 0) {
            if (resume_job(&tokens, &jobs, 0) == -1) {
                printf("Failed to resume job in background\n");
            }
        }

        // Task 6: Wait for a specific job identified by its index in job list
        else if (strcmp(first_token, "wait-for") == 0) {
            if (await_background_job(&tokens, &jobs) == -1) {
                printf("Failed to wait for background job\n");
            }
        }

        // Task 6: Wait for all background jobs
        else if (strcmp(first_token, "wait-all") == 0) {
            if (await_all_background_jobs(&jobs) == -1) {
                printf("Failed to wait for all background jobs\n");
            }
        }

        else {
            // parent process
            pid_t pid = fork();
            int status;

            if (pid == 0) {
                // child process
                if (run_command(&tokens) == -1) {
                    // child exits on failure
                    exit(1);
                }
            } else if (pid > 0) {
                //Check if the command is intended to be run in the background
                if (tokens.length > 0 && strcmp(strvec_get(&tokens, tokens.length - 1), "&") == 0) {
                    // Remove the "&" from the token list
                    strvec_take(&tokens, tokens.length - 1);

                    // Set the child process group
                    if (setpgid(pid, pid) == -1) {
                        perror("setpgid");
                    }

                    // Add the job to the jobs list with status BACKGROUND
                    job_list_add(&jobs, pid, strvec_get(&tokens, 0), BACKGROUND);
                } else {
                    // Foreground job handling
                    // Set the terminal's process group to the child's PID
                    if (tcsetpgrp(STDIN_FILENO, pid) == -1) {
                        perror("tcsetpgrp");
                    }

                    // Wait for the child process to finish or be stopped
                    if (waitpid(pid, &status, WUNTRACED) == -1) {
                        perror("waitpid");
                    }

                    // Restore the shell to the foreground
                    if (tcsetpgrp(STDIN_FILENO, getpid()) == -1) {
                        perror("tcsetpgrp");
                    }

                    // Handle stopped process
                    if (WIFSTOPPED(status)) {
                        // Add job to the job list with STOPPED status
                        job_list_add(&jobs, pid, strvec_get(&tokens, 0), STOPPED);
                    }
                }
            } else {
                // error on fork
                perror("fork");
            }
        }
        strvec_clear(&tokens);
        printf("%s", PROMPT);
    }
    job_list_free(&jobs);
    return 0;
}
