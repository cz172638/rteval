/*
 * Copyright (C) 2009 Red Hat Inc.
 *
 * This application is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 2.
 *
 * This application is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

/**
 * @file   rteval_parserd.c
 * @author David Sommerseth <davids@redhat.com>
 * @date   Thu Oct 15 11:59:27 2009
 *
 * @brief  Polls the rteval.submissionqueue table for notifications
 *         from new inserts and sends the file to a processing thread
 *
 *
 *
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>

#include <eurephia_nullsafe.h>
#include <eurephia_values.h>
#include <configparser.h>
#include <pgsql.h>
#include <threadinfo.h>
#include <parsethread.h>

#define DEFAULT_MSG_MAX 5
#define XMLPARSER_XSL "xmlparser.xsl"

static int shutdown = 0; /**<  Global variable indicating if the program should shutdown */

/**
 * Simple signal catcher.  Used for SIGINT and SIGTERM signals, and will set the global shutdown
 * shutdown flag.  It's expected that all threads behaves properly and exits as soon as their current
 * work is completed
 *
 * @param sig Recieved signal (not used)
 */
void sigcatch(int sig) {
	if( shutdown == 0 ) {
		shutdown = 1;
		fprintf(stderr, "** SIGNAL ** Starting shutting down\n");
	} else {
		fprintf(stderr, "** SIGNAL ** Shutdown in progress ... please be patient ...\n");
	}

	// re-enable signals, to avoid brute force exits.
	// If brute force is needed, SIGKILL is available.
	signal(SIGINT, sigcatch);
	signal(SIGTERM, sigcatch);
}


/**
 * Opens and reads /proc/sys/fs/mqueue/msg_max, to get the maximum number of allowed messages
 * on POSIX MQ queues.  rteval_parserd will use as much of this as possible when needed.
 *
 * @return Returns the system msg_max value, or DEFAULT_MSG_MAX on failure to read the setting.
 */
unsigned int get_mqueue_msg_max() {
	FILE *fp = NULL;
	char buf[130];
	unsigned int msg_max = DEFAULT_MSG_MAX;

	fp = fopen("/proc/sys/fs/mqueue/msg_max", "r");
	if( !fp ) {
		fprintf(stderr,
			"** ERROR **  Could not open /proc/sys/fs/mqueue/msg_max, defaulting to %i\n",
			msg_max);
		fprintf(stderr, "** ERROR **  %s\n", strerror(errno));
		return msg_max;
	}

	memset(&buf, 0, 130);
	if( fread(&buf, 1, 128, fp) < 1 ) {
		fprintf(stderr,
			"** ERROR **  Could not read /proc/sys/fs/mqueue/msg_max, defaulting to %i\n",
			msg_max);
		fprintf(stderr, "** ERROR **  %s\n", strerror(errno));
	} else {
		msg_max = atoi_nullsafe(buf);
		if( msg_max < 1 ) {
			msg_max = DEFAULT_MSG_MAX;
			fprintf(stderr,
				"** ERROR **  Failed to parse /proc/sys/fs/mqueue/msg_max,"
				"defaulting to %i\n", msg_max);
		}
	}
	fclose(fp);
	return msg_max;
}


/**
 * Main loop, which polls the submissionqueue table and puts jobs found here into a POSIX MQ queue
 * which the worker threads will pick up.
 *
 * @param dbc    Database connection, where to query the submission queue
 * @param msgq   file descriptor for the message queue
 *
 * @return Returns 0 on successful run, otherwise > 0 on errors.
 */
int process_submission_queue(dbconn *dbc, mqd_t msgq) {
	pthread_mutex_t mtx_submq = PTHREAD_MUTEX_INITIALIZER;
	parseJob_t *job = NULL;
	int rc = 0;

	while( shutdown == 0 ) {
		// Fetch an available job
		job = db_get_submissionqueue_job(dbc, &mtx_submq);
		if( !job ) {
			fprintf(stderr, "** ERROR **  Failed to get submission queue job - shutting down\n");
			shutdown = 1;
			rc = 1;
			goto exit;
		}
		if( job->status == jbNONE ) {
			free_nullsafe(job);
			if( db_wait_notification(dbc, &shutdown, "rteval_submq") < 1 ) {
				fprintf(stderr, "** ERROR **  Failed to wait for DB notification\n");
				shutdown = 1;
				rc = 1;
				goto exit;
			}
			continue;
		}

		// Send the job to the queue
		fprintf(stderr, "** New job: submid %i, %s\n", job->submid, job->filename);
		do {
			int res;

			errno = 0;
			res = mq_send(msgq, (char *) job, sizeof(parseJob_t), 1);
			if( (res < 0) && (errno != EAGAIN) ) {
				fprintf(stderr, "** ERROR **  Could not send parse job to the queue\n");
				shutdown = 1;
				rc = 2;
				goto exit;
			} else if( errno == EAGAIN ) {
				fprintf(stderr,
					"** WARNING **  Message queue filled up.  "
					"Will not add new messages to queue for the next 60 seconds\n");
				sleep(60);
			}
		} while( (errno == EAGAIN) );
		free_nullsafe(job);
	}
 exit:
	return rc;
}


/**
 * rtevald_parser main function.
 *
 * @param argc
 * @param argv
 *
 * @return Returns the result of the process_submission_queue() function.
 */
int main(int argc, char **argv) {
        eurephiaVALUES *config = NULL;
        char xsltfile[2050], *reportdir = NULL;
	xsltStylesheet *xslt = NULL;
	dbconn *dbc = NULL;
        pthread_t **threads = NULL;
        pthread_attr_t **thread_attrs = NULL;
	pthread_mutex_t mtx_sysreg = PTHREAD_MUTEX_INITIALIZER;
	threadData_t **thrdata;
	struct mq_attr msgq_attr;
	mqd_t msgq;
	int i,rc, max_threads = 5;

	// Initialise XML and XSLT libraries
	xsltInit();
	xmlInitParser();

	// Fetch configuration
        config = read_config("/etc/rteval.conf", "xmlrpc_parser");

	// Parse XSLT template
	snprintf(xsltfile, 512, "%s/%s", eGet_value(config, "xsltpath"), XMLPARSER_XSL);
        xslt = xsltParseStylesheetFile((xmlChar *) xsltfile);
	if( !xslt ) {
		fprintf(stderr, "** ERROR **  Could not parse XSLT template: %s\n", xsltfile);
		rc = 2;
		goto exit;
	}

	// Open a POSIX MQ
	memset(&msgq, 0, sizeof(mqd_t));
	msgq_attr.mq_maxmsg = get_mqueue_msg_max();
	msgq_attr.mq_msgsize = sizeof(parseJob_t);
	msgq_attr.mq_flags = O_NONBLOCK;
	msgq = mq_open("/rteval_parsequeue", O_RDWR | O_CREAT | O_NONBLOCK, 0600, &msgq_attr);
	if( msgq < 0 ) {
		fprintf(stderr, "** ERROR **  Could not open message queue: %s\n", strerror(errno));
		rc = 2;
		goto exit;
	}

	// Get a database connection for the main thread
        dbc = db_connect(config);
        if( !dbc ) {
		rc = 2;
		goto exit;
        }

	// Prepare all threads
	threads = calloc(max_threads + 1, sizeof(pthread_t *));
	thread_attrs = calloc(max_threads + 1, sizeof(pthread_attr_t *));
	thrdata = calloc(max_threads + 1, sizeof(threadData_t *));
	assert( (threads != NULL) && (thread_attrs != NULL) && (thrdata != NULL) );

	reportdir = eGet_value(config, "reportdir");
	for( i = 0; i < max_threads; i++ ) {
		// Prepare thread specific data
		thrdata[i] = malloc_nullsafe(sizeof(threadData_t));
		if( !thrdata[i] ) {
			fprintf(stderr, "** ERROR **  Could not allocate memory for thread data\n");
			rc = 2;
			goto exit;
		}

		// Get a database connection for the thread
		thrdata[i]->dbc = db_connect(config);
		if( !thrdata[i]->dbc ) {
			fprintf(stderr,
				"** ERROR **  Could not connect to the database for thread %i\n", i);
			rc = 2;
			shutdown = 1;
			goto exit;
		}

		thrdata[i]->shutdown = &shutdown;
		thrdata[i]->id = i;
		thrdata[i]->msgq = msgq;
		thrdata[i]->mtx_sysreg = &mtx_sysreg;
		thrdata[i]->xslt = xslt;
		thrdata[i]->destdir = reportdir;

		thread_attrs[i] = malloc_nullsafe(sizeof(pthread_attr_t));
		if( !thread_attrs[i] ) {
			fprintf(stderr, "** ERROR **  Could not allocate memory for thread attributes\n");
			rc = 2;
			goto exit;
		}
		pthread_attr_init(thread_attrs[i]);
		pthread_attr_setdetachstate(thread_attrs[i], PTHREAD_CREATE_JOINABLE);

		threads[i] = malloc_nullsafe(sizeof(pthread_t));
		if( !threads[i] ) {
			fprintf(stderr, "** ERROR **  Could not allocate memory for pthread_t\n");
			rc = 2;
			goto exit;
		}
	}

	// Setup signal catchers
	signal(SIGINT, sigcatch);
	signal(SIGTERM, sigcatch);

	// Start the threads
	for( i = 0; i < max_threads; i++ ) {
		int thr_rc = pthread_create(threads[i], thread_attrs[i], parsethread, thrdata[i]);
		if( thr_rc < 0 ) {
			fprintf(stderr, "** ERROR **  Failed to start thread %i: %s",
				i, strerror(thr_rc));
			rc = 3;
			goto exit;
		}
	}

	// Main routine
	//
	// checks the submission queue and puts unprocessed records on the POSIX MQ
	// to be parsed by one of the threads
	//
	fprintf(stderr, "** Starting submission queue checker\n");
	rc = process_submission_queue(dbc, msgq);
	fprintf(stderr, "** Submission queue checker shut down\n");

 exit:
	// Clean up all threads
	for( i = 0; i < max_threads; i++ ) {
		// Wait for all threads to exit
		if( threads && threads[i] ) {
			void *thread_rc;
			int j_rc;

			if( (j_rc = pthread_join(*threads[i], &thread_rc)) != 0 ) {
				fprintf(stderr, "** ERROR **  Failed to join thread %i: %s\n", 
					i, strerror(j_rc));
			}
			pthread_attr_destroy(thread_attrs[i]);
			free_nullsafe(threads[i]);
			free_nullsafe(thread_attrs[i]);
		}

		// Disconnect threads database connection
		if( thrdata && thrdata[i] ) {
			db_disconnect(thrdata[i]->dbc);
			free_nullsafe(thrdata[i]);
		}
	}
	free_nullsafe(thrdata);
	free_nullsafe(threads);
	free_nullsafe(thread_attrs);

	// Close message queue
	errno = 0;
	if( mq_close(msgq) < 0 ) {
		fprintf(stderr, "** ERROR **  Failed to close message queue: %s\n",
			strerror(errno));
	}
	errno = 0;
	if( mq_unlink("/rteval_parsequeue") < 0 ) {
		fprintf(stderr, "** ERROR **  Failed to remove the message queue: %s\n",
			strerror(errno));
	}

	// Disconnect from database, main thread connection
	db_disconnect(dbc);

	// Free up the rest
	eFree_values(config);
	xsltFreeStylesheet(xslt);
	xmlCleanupParser();
	xsltCleanupGlobals();

	return rc;
}

