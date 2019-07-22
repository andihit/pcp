#ifndef _TEST_TARGET

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <chan/chan.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <pcp/pmapi.h>

#include "statsd.h"
#include "config-reader.h"
#include "network-listener.h"
#include "aggregators.h"
#include "aggregator-metrics.h"
#include "aggregator-stats.h"
#include "pmda-callbacks.h"
#include "utils.h"
#include "../domain.h"

void signal_handler(int num) {
    if (num == SIGUSR1) {
        aggregator_request_output();
    }
}

/**
 * Initializes structure which is used as private data container accross all PCP related callbacks
 * @arg args - All args passed to 'PCP exchange' thread
 */
static void
init_data_ext(
    struct pmda_data_extension* data,
    struct agent_config* config,
    struct pmda_metrics_container* metrics_storage,
    struct pmda_stats_container* stats_storage
) {
    data->config = config;
    data->pcp_metric_count = 0;
    data->metrics_storage = metrics_storage;
    data->stats_storage =stats_storage;
}

/**
 * Registers hardcoded metrics that are registered before PMDA agent initializes itself fully
 * @arg pi - pmdaInterface carries all PCP stuff
 * @arg data - 
 */
static void
create_statsd_hardcoded_metrics(pmdaInterface* pi, struct pmda_data_extension* data) {
    size_t i;
    size_t hardcoded_count = 6;
    data->pcp_metrics = malloc(hardcoded_count * sizeof(pmdaMetric));
    ALLOC_CHECK("Unable to allocate space for static PMDA metrics.");
    for (i = 0; i < hardcoded_count; i++) {
        data->pcp_metrics[i].m_user = data;
        data->pcp_metrics[i].m_desc.pmid = pmID_build(pi->domain, 0, i);
        data->pcp_metrics[i].m_desc.type = PM_TYPE_U64;
        data->pcp_metrics[i].m_desc.indom = PM_INDOM_NULL;
        data->pcp_metrics[i].m_desc.sem = PM_SEM_INSTANT;
        if (i < 4) {
            memset(&data->pcp_metrics[i].m_desc.units, 0, sizeof(pmUnits));
        } else {
            // time_spent_parsing / time_spent_aggregating
            data->pcp_metrics[i].m_desc.units.dimSpace = 0;
            data->pcp_metrics[i].m_desc.units.dimTime = 0;
            data->pcp_metrics[i].m_desc.units.dimCount = 0;
            data->pcp_metrics[i].m_desc.units.pad = 0;
            data->pcp_metrics[i].m_desc.units.scaleSpace = 0;
            data->pcp_metrics[i].m_desc.units.scaleTime = PM_TIME_NSEC;
            data->pcp_metrics[i].m_desc.units.scaleCount = 1;
        }
    }
    data->pcp_metric_count = 6;
}

int
main(int argc, char** argv)
{
    signal(SIGUSR1, signal_handler);

    struct agent_config config;
    struct pmda_data_extension data;
    pthread_t network_listener;
    pthread_t parser;
    pthread_t aggregator;
    pmdaInterface dispatch;

    int sep = pmPathSeparator();
    char config_file_path[MAXPATHLEN];
    char help_file_path[MAXPATHLEN];
    pmsprintf(
        config_file_path,
        MAXPATHLEN,
        "%s" "%c" "statsd" "%c" "statsd-pmda.ini",
        pmGetConfig("PCP_PMDAS_DIR"),
        sep, sep);
    pmsprintf(
        help_file_path,
        MAXPATHLEN,
        "%s%c" "statsd" "%c" "help",
        pmGetConfig("PCP_PMDAS_DIR"),
        sep, sep);

    pmSetProgname(argv[0]);
    pmdaDaemon(&dispatch, PMDA_INTERFACE_7, pmGetProgname(), STATSD, "statsd.log", help_file_path);

    read_agent_config(&config, &dispatch, config_file_path, argc, argv);
    if (config.debug) {
        print_agent_config(&config);
    }

    struct pmda_metrics_container* metrics = init_pmda_metrics(&config);
    struct pmda_stats_container* stats = init_pmda_stats(&config);
    init_loggers(&config);
    init_data_ext(&data, &config, metrics, stats);

    chan_t* network_listener_to_parser = chan_init(config.max_unprocessed_packets);
    if (network_listener_to_parser == NULL) DIE("Unable to create channel network listener -> parser.");
    chan_t* parser_to_aggregator = chan_init(config.max_unprocessed_packets);
    if (parser_to_aggregator == NULL) DIE("Unable to create channel parser -> aggregator.");

    struct network_listener_args* listener_args = create_listener_args(&config, network_listener_to_parser);
    struct parser_args* parser_args = create_parser_args(&config, network_listener_to_parser, parser_to_aggregator);
    struct aggregator_args* aggregator_args = create_aggregator_args(&config, parser_to_aggregator, metrics, stats);

    int pthread_errno = 0; 
    pthread_errno = pthread_create(&network_listener, NULL, network_listener_exec, listener_args);
    PTHREAD_CHECK(pthread_errno);
    pthread_errno = pthread_create(&parser, NULL, parser_exec, parser_args);
    PTHREAD_CHECK(pthread_errno);
    pthread_errno = pthread_create(&aggregator, NULL, aggregator_exec, aggregator_args);
    PTHREAD_CHECK(pthread_errno);

    pmdaOpenLog(&dispatch);
    pmSetProcessIdentity(config.username);
    if (dispatch.status != 0) {
        pthread_exit(NULL);
    }
    dispatch.version.seven.fetch = statsd_fetch;
	dispatch.version.seven.store = statsd_store;
	dispatch.version.seven.desc = statsd_desc;
	dispatch.version.seven.text = statsd_text;
	dispatch.version.seven.instance = statsd_instance;
	// dispatch.version.seven.pmid = statsd_pmid;
	// dispatch.version.seven.name = statsd_name;
	// dispatch.version.seven.children = statsd_children;
	dispatch.version.seven.label = statsd_label;
    // Callbacks
	pmdaSetFetchCallBack(&dispatch, statsd_fetch_callback);
    pmdaSetLabelCallBack(&dispatch, statsd_label_callback);

    create_statsd_hardcoded_metrics(&dispatch, &data);
    pmdaSetData(&dispatch, (void*) &data);
    pmdaSetFlags(&dispatch, PMDA_EXT_FLAG_HASHED);
    pmdaInit(&dispatch, NULL, 0, data.pcp_metrics, data.pcp_metric_count);
    pmdaConnect(&dispatch);
    pmdaMain(&dispatch);

    if (pthread_join(network_listener, NULL) != 0) {
        DIE("Error joining network network listener thread.");
    }
    if (pthread_join(parser, NULL) != 0) {
        DIE("Error joining datagram parser thread.");
    }
    if (pthread_join(aggregator, NULL) != 0) {
        DIE("Error joining datagram aggregator thread.");
    }

    chan_close(network_listener_to_parser);
    chan_close(parser_to_aggregator);
    chan_dispose(network_listener_to_parser);
    chan_dispose(parser_to_aggregator);
    return EXIT_SUCCESS;
}

#endif
