/* SPDX-License-Identifier: LGPL-2.1+
 * Copyright © 2019 VMware, Inc. */

#include <linux/pkt_sched.h>

#include "alloc-util.h"
#include "conf-parser.h"
#include "in-addr-util.h"
#include "netlink-util.h"
#include "networkd-manager.h"
#include "parse-util.h"
#include "qdisc.h"
#include "set.h"
#include "string-util.h"

static int qdisc_new(QDisc **ret) {
        QDisc *qdisc;

        qdisc = new(QDisc, 1);
        if (!qdisc)
                return -ENOMEM;

        *qdisc = (QDisc) {
                .family = AF_UNSPEC,
                .parent = TC_H_ROOT,
        };

        *ret = TAKE_PTR(qdisc);

        return 0;
}

int qdisc_new_static(Network *network, const char *filename, unsigned section_line, QDisc **ret) {
        _cleanup_(network_config_section_freep) NetworkConfigSection *n = NULL;
        _cleanup_(qdisc_freep) QDisc *qdisc = NULL;
        int r;

        assert(network);
        assert(ret);
        assert(!!filename == (section_line > 0));

        if (filename) {
                r = network_config_section_new(filename, section_line, &n);
                if (r < 0)
                        return r;

                qdisc = ordered_hashmap_get(network->qdiscs_by_section, n);
                if (qdisc) {
                        *ret = TAKE_PTR(qdisc);

                        return 0;
                }
        }

        r = qdisc_new(&qdisc);
        if (r < 0)
                return r;

        qdisc->network = network;

        if (filename) {
                qdisc->section = TAKE_PTR(n);

                r = ordered_hashmap_ensure_allocated(&network->qdiscs_by_section, &network_config_hash_ops);
                if (r < 0)
                        return r;

                r = ordered_hashmap_put(network->qdiscs_by_section, qdisc->section, qdisc);
                if (r < 0)
                        return r;
        }

        *ret = TAKE_PTR(qdisc);

        return 0;
}

void qdisc_free(QDisc *qdisc) {
        if (!qdisc)
                return;

        if (qdisc->network && qdisc->section)
                ordered_hashmap_remove(qdisc->network->qdiscs_by_section, qdisc->section);

        network_config_section_free(qdisc->section);

        free(qdisc->tca_kind);
        free(qdisc);
}

static int qdisc_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        assert(link);
        assert(link->qdisc_messages > 0);
        link->qdisc_messages--;

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 1;

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EEXIST) {
                log_link_message_error_errno(link, m, r, "Could not set QDisc: %m");
                link_enter_failed(link);
                return 1;
        }

        if (link->qdisc_messages == 0) {
                log_link_debug(link, "QDisc configured");
                link->qdiscs_configured = true;
                link_check_ready(link);
        }

        return 1;
}

int qdisc_configure(Link *link, QDisc *qdisc) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL;
        _cleanup_free_ char *tca_kind = NULL;
        char *p;
        int r;

        assert(link);
        assert(link->manager);
        assert(link->manager->rtnl);
        assert(link->ifindex > 0);

        r = sd_rtnl_message_new_qdisc(link->manager->rtnl, &req, RTM_NEWQDISC, qdisc->family, link->ifindex);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not create RTM_NEWQDISC message: %m");

        r = sd_rtnl_message_set_qdisc_parent(req, qdisc->parent);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not create tcm_parent message: %m");

        if (qdisc->handle != TC_H_UNSPEC) {
                r = sd_rtnl_message_set_qdisc_handle(req, qdisc->handle);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not set tcm_handle message: %m");
        }

        if (qdisc->has_network_emulator) {
                r = free_and_strdup(&tca_kind, "netem");
                if (r < 0)
                        return log_oom();

                r = network_emulator_fill_message(link, &qdisc->ne, req);
                if (r < 0)
                        return r;
        }

        if (qdisc->has_token_buffer_filter) {
                r = free_and_strdup(&tca_kind, "tbf");
                if (r < 0)
                        return log_oom();

                r = token_buffer_filter_fill_message(link, &qdisc->tbf, req);
                if (r < 0)
                        return r;
        }

        if (qdisc->has_stochastic_fairness_queueing) {
                r = free_and_strdup(&tca_kind, "sfq");
                if (r < 0)
                        return log_oom();

                r = stochastic_fairness_queueing_fill_message(link, &qdisc->sfq, req);
                if (r < 0)
                        return r;
        }

        if (qdisc->has_fair_queuing_controlled_delay) {
                r = free_and_strdup(&tca_kind, "fq_codel");
                if (r < 0)
                        return log_oom();

                r = fair_queuing_controlled_delay_fill_message(link, &qdisc->fq_codel, req);
                if (r < 0)
                        return r;
        }

        p = tca_kind ?:qdisc->tca_kind;
        if (p) {
                r = sd_netlink_message_append_string(req, TCA_KIND, p);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append TCA_KIND attribute: %m");
        }

        r = netlink_call_async(link->manager->rtnl, NULL, req, qdisc_handler, link_netlink_destroy_callback, link);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not send rtnetlink message: %m");

        link_ref(link);
        link->qdisc_messages++;

        return 0;
}

int qdisc_section_verify(QDisc *qdisc, bool *has_root, bool *has_clsact) {
        unsigned i;
        int r;

        assert(qdisc);
        assert(has_root);
        assert(has_clsact);

        if (section_is_invalid(qdisc->section))
                return -EINVAL;

        i = qdisc->has_network_emulator + qdisc->has_token_buffer_filter + qdisc->has_stochastic_fairness_queueing;
        if (i > 1)
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: TrafficControlQueueingDiscipline section has more than one type of discipline. "
                                         "Ignoring [TrafficControlQueueingDiscipline] section from line %u.",
                                         qdisc->section->filename, qdisc->section->line);

        if (qdisc->has_token_buffer_filter) {
                r = token_buffer_filter_section_verify(&qdisc->tbf, qdisc->section);
                if (r < 0)
                        return r;
        }

        if (qdisc->parent == TC_H_ROOT) {
                if (*has_root)
                        return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                                 "%s: More than one root TrafficControlQueueingDiscipline sections are defined. "
                                                 "Ignoring [TrafficControlQueueingDiscipline] section from line %u.",
                                                 qdisc->section->filename, qdisc->section->line);
                *has_root = true;
        } else if (qdisc->parent == TC_H_CLSACT) { /* TC_H_CLSACT == TC_H_INGRESS */
                if (*has_clsact)
                        return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                                 "%s: More than one clsact or ingress TrafficControlQueueingDiscipline sections are defined. "
                                                 "Ignoring [TrafficControlQueueingDiscipline] section from line %u.",
                                                 qdisc->section->filename, qdisc->section->line);
                *has_clsact = true;
        }

        return 0;
}

int config_parse_tc_qdiscs_parent(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(qdisc_free_or_set_invalidp) QDisc *qdisc = NULL;
        Network *network = data;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = qdisc_new_static(network, filename, section_line, &qdisc);
        if (r < 0)
                return r;

        if (streq(rvalue, "root")) {
                qdisc->parent = TC_H_ROOT;
                qdisc->handle = TC_H_UNSPEC;
        } else if (streq(rvalue, "clsact")) {
                qdisc->parent = TC_H_CLSACT;
                qdisc->handle = TC_H_MAKE(TC_H_CLSACT, 0);
        } else if (streq(rvalue, "ingress")) {
                qdisc->parent = TC_H_INGRESS;
                qdisc->handle = TC_H_MAKE(TC_H_INGRESS, 0);
        } else {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Failed to parse 'Parent=', ignoring assignment: %s",
                           rvalue);
                return 0;
        }

        if (streq(rvalue, "root"))
                qdisc->tca_kind = mfree(qdisc->tca_kind);
        else {
                r = free_and_strdup(&qdisc->tca_kind, rvalue);
                if (r < 0)
                        return log_oom();
        }

        qdisc = NULL;

        return 0;
}
