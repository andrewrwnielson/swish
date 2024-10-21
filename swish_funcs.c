#define _GNU_SOURCE

#include "swish_funcs.h"

#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "job_list.h"
#include "string_vector.h"

#define MAX_ARGS 10

int tokenize(char *s, strvec_t *tokens) {
    // Tokenize string s with space as delimeter
    // Add each token to the 'tokens' parameter (a string vector)
    // Return 0 on success, -1 on error
    char *tok = strtok(s, " ");
    while (tok != NULL) {
        if (strvec_add(tokens, tok) == -1) {
            printf("Failed to add token to tokens string vector\n");
            return -1;
        }
        tok = strtok(NULL, " ");
    }
    return 0;
}

int run_command(strvec_t *tokens) {
    // program to be ran
    char *program = strvec_get(tokens, 0);

    // command-line arguments for program
    char *arguments[MAX_ARGS + 1];

    // current token from tokens
    // add tokens but exlude redirect operators
    char *i_token;
    int i = 0;
    while ((i_token = strvec_get(tokens, i)) != NULL && i < MAX_ARGS - 1) {
        // add current token to arguments array if it is not a redirect operator
        if (strcmp(i_token, "<") == 0 || strcmp(i_token, ">") == 0 || strcmp(i_token, ">>") == 0) {
            break;
        }
        arguments[i] = i_token;
        i++;
    }
    // NULL sentinel
    arguments[i] = NULL;

    int fd;
    // check for redirection
    for (i = 0; strvec_get(tokens, i) != NULL; i++) {
        if (strcmp(strvec_get(tokens, i), "<") == 0) {
            // open file for reading
            fd = open(strvec_get(tokens, i + 1), O_RDONLY);
            if (fd == -1) {
                perror("Failed to open input file");
                return -1;
            }
            // redirect stdin
            if (dup2(fd, STDIN_FILENO) == -1) {
                perror("dup2");
                close(fd);
                return -1;
            }
            close(fd);
        } else if (strcmp(strvec_get(tokens, i), ">") == 0) {
            // open file for writing
            fd = open(strvec_get(tokens, i + 1), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
            if (fd == -1) {
                perror("Failed to open output file");
                return -1;
            }
            // redirect stdout
            if (dup2(fd, STDOUT_FILENO) == -1) {
                perror("dup2");
                close(fd);
                return -1;
            }
            close(fd);
        } else if (strcmp(strvec_get(tokens, i), ">>") == 0) {
            // open file for appending
            fd = open(strvec_get(tokens, i + 1), O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR);
            if (fd == -1) {
                perror("Failed to open output file");
                return -1;
            }
            // redirect stdout
            if (dup2(fd, STDOUT_FILENO) == -1) {
                perror("dup2");
                close(fd);
                return -1;
            }
            close(fd);
        }
    }

    // reset signal handlers
    struct sigaction sac;
    sac.sa_handler = SIG_DFL;
    // block all signals
    if (sigemptyset(&sac.sa_mask) == -1) {
        perror("sigemptyset");
        return -1;
    }
    sac.sa_flags = 0;
    // ignore SIGTTIN and SIGTTOU
    if (sigaction(SIGTTIN, &sac, NULL) == -1 || sigaction(SIGTTOU, &sac, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    pid_t pid = getpid();
    // set process group id to pid
    if (setpgid(0, pid) == -1) {
        perror("setpgid");
        return -1;
    }

    execvp(program, arguments);

    // if exec returns then an error has occured
    perror("exec");
    return -1;
}

int resume_job(strvec_t *tokens, job_list_t *jobs, int is_foreground) {
    // Check if job number is provided
    if (tokens->length < 2) {
        fprintf(stderr, "Usage: %s <job number>\n", is_foreground ? "fg" : "bg");
        return -1;
    }

    // Parse job number from tokens
    int job_num;
    if (sscanf(strvec_get(tokens, 1), "%d", &job_num) != 1) {
        fprintf(stderr, "Invalid job number\n");
        return -1;
    }

    // Retrieve job from job list
    job_t *job = job_list_get(jobs, job_num);
    if (job == NULL) {
        fprintf(stderr, "Job index out of bounds\n");
        return -1;
    }

    // Ensure job is currently stopped
    if (job->status != STOPPED) {
        fprintf(stderr, "Job is not stopped\n");
        return -1;
    }

    // Bring job to foreground if needed
    if (is_foreground) {
        if (tcsetpgrp(STDIN_FILENO, job->pid) == -1) {
            perror("tcsetpgrp");
            return -1;
        }
    }

    // Send SIGCONT to resume the job
    if (kill(job->pid, SIGCONT) == -1) {
        perror("kill");
        return -1;
    }

    // Foreground handling
    if (is_foreground) {
        // Wait for the job to stop
        int status;
        if (waitpid(job->pid, &status, WUNTRACED) == -1) {
            perror("waitpid");
            return -1;
        }

        // Check if job was stopped again
        if (WIFSTOPPED(status)) {
            job->status = STOPPED;
        } else {
            // Remove job from the list if it has terminated
            job_list_remove(jobs, job_num);
        }

        // Restore the shell process to the foreground
        if (tcsetpgrp(STDIN_FILENO, getpid()) == -1) {
            perror("tcsetpgrp");
            return -1;
        }
    } else {
        // update job status to BACKGROUND if it is not foreground
        job->status = BACKGROUND;
    }

    return 0;
}

int await_background_job(strvec_t *tokens, job_list_t *jobs) {
    // Check if job number is provided
    if (tokens->length < 2) {
        fprintf(stderr, "Usage: wait-for <job number>\n");
        return -1;
    }

    // Parse job number from tokens
    int job_num;
    if (sscanf(strvec_get(tokens, 1), "%d", &job_num) != 1) {
        fprintf(stderr, "Invalid job number\n");
        return -1;
    }

    // Retrieve job from job list
    job_t *job = job_list_get(jobs, job_num);
    if (job == NULL) {
        fprintf(stderr, "Job not found\n");
        return -1;
    }

    // Ensure job is running in the background
    if (job->status != BACKGROUND) {
        fprintf(stderr, "Job index is for stopped process not background process\n");
        return -1;
    }

    // Wait for the job to terminate or stop
    int status;
    if (waitpid(job->pid, &status, WUNTRACED) == -1) {
        perror("waitpid");
        return -1;
    }

    // Update job status
    if (WIFSTOPPED(status)) {
        job->status = STOPPED;
    } else {
        // Remove job from the list if it has terminated
        job_list_remove(jobs, job_num);
    }

    return 0;
}

int await_all_background_jobs(job_list_t *jobs) {
    job_t *current = jobs->head;
    job_t *next_job;

    // Loop through jobs
    while (current != NULL) {
        next_job = current->next;

        // Only check background jobs
        if (current->status == BACKGROUND) {
            int status;

            // Wait for the background job to stop or terminate
            if (waitpid(current->pid, &status, WUNTRACED) == -1) {
                perror("waitpid");
                return -1;
            }

            // If stopped change status
            if (WIFSTOPPED(status)) {
                current->status = STOPPED;
            } else {
                // Remove job from the list if it has terminated
                int idx = 0;
                job_t *temp = jobs->head;
                while (temp != current) {
                    temp = temp->next;
                    idx++;
                }
                job_list_remove(jobs, idx);
            }
        }

        current = next_job;
    }

    return 0;
}
