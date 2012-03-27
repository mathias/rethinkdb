#include "unittest/gtest.hpp"
#include "clustering/immediate_consistency/branch/broadcaster.hpp"
#include "clustering/immediate_consistency/branch/listener.hpp"
#include "clustering/immediate_consistency/branch/replier.hpp"
#include "memcached/protocol.hpp"
#include "unittest/dummy_metadata_controller.hpp"
#include "unittest/clustering_utils.hpp"
#include "unittest/unittest_utils.hpp"

namespace unittest {

namespace {

void run_with_broadcaster(
        boost::function< void(
            simple_mailbox_cluster_t *,
            boost::shared_ptr<semilattice_readwrite_view_t<branch_history_t<memcached_protocol_t> > >,
            clone_ptr_t<directory_single_rview_t<boost::optional<broadcaster_business_card_t<memcached_protocol_t> > > >,
            boost::scoped_ptr<broadcaster_t<memcached_protocol_t> > *,
            test_store_t<memcached_protocol_t> *,
            boost::scoped_ptr<listener_t<memcached_protocol_t> > *
            )> fun)
{
    /* Set up a cluster so mailboxes can be created */
    simple_mailbox_cluster_t cluster;

    /* Set up metadata meeting-places */
    branch_history_t<memcached_protocol_t> initial_branch_history;
    dummy_semilattice_controller_t<branch_history_t<memcached_protocol_t> >
        branch_history_controller(initial_branch_history);

    /* Set up a broadcaster and initial listener */
    test_store_t<memcached_protocol_t> initial_store;
    cond_t interruptor;

    boost::scoped_ptr<broadcaster_t<memcached_protocol_t> > broadcaster(
        new broadcaster_t<memcached_protocol_t>(
            cluster.get_mailbox_manager(),
            branch_history_controller.get_view(),
            &initial_store.store,
            &interruptor
        ));

    simple_directory_manager_t<boost::optional<broadcaster_business_card_t<memcached_protocol_t> > >
        broadcaster_metadata_controller(&cluster,
            boost::optional<broadcaster_business_card_t<memcached_protocol_t> >(broadcaster->get_business_card())
            );

    boost::scoped_ptr<listener_t<memcached_protocol_t> > initial_listener(
        new listener_t<memcached_protocol_t>(
            cluster.get_mailbox_manager(),
            broadcaster_metadata_controller.get_root_view()->
                get_peer_view(cluster.get_connectivity_service()->get_me()),
            branch_history_controller.get_view(),
            broadcaster.get(),
            &interruptor
        ));

    fun(&cluster,
        branch_history_controller.get_view(),
        broadcaster_metadata_controller.get_root_view()->
            get_peer_view(cluster.get_connectivity_service()->get_me()),
        &broadcaster,
        &initial_store,
        &initial_listener);
}

void run_in_thread_pool_with_broadcaster(
        boost::function< void(
            simple_mailbox_cluster_t *,
            boost::shared_ptr<semilattice_readwrite_view_t<branch_history_t<memcached_protocol_t> > >,
            clone_ptr_t<directory_single_rview_t<boost::optional<broadcaster_business_card_t<memcached_protocol_t> > > >,
            boost::scoped_ptr<broadcaster_t<memcached_protocol_t> > *,
            test_store_t<memcached_protocol_t> *,
            boost::scoped_ptr<listener_t<memcached_protocol_t> > *
            )> fun)
{
    run_in_thread_pool(boost::bind(&run_with_broadcaster, fun));
}

/* `let_stuff_happen()` delays for some time to let events occur */
void let_stuff_happen() {
    nap(1000);
}

}   /* anonymous namespace */

/* `PartialBackfill` backfills only in a specific sub-region. */

static void write_to_broadcaster(broadcaster_t<memcached_protocol_t> *broadcaster, const std::string& key, const std::string& value, order_token_t otok, signal_t *) {
    sarc_mutation_t set;
    set.key = store_key_t(key);
    set.data = data_buffer_t::create(value.size());
    memcpy(set.data->buf(), value.c_str(), value.size());
    set.flags = 123;
    set.exptime = 0;
    set.add_policy = add_policy_yes;
    set.replace_policy = replace_policy_yes;
    memcached_protocol_t::write_t write(set, time(NULL), 12345);
    broadcaster->write(write, otok);
}

void run_partial_backfill_test(simple_mailbox_cluster_t *cluster,
        boost::shared_ptr<semilattice_readwrite_view_t<branch_history_t<memcached_protocol_t> > > branch_history_view,
        clone_ptr_t<directory_single_rview_t<boost::optional<broadcaster_business_card_t<memcached_protocol_t> > > > broadcaster_metadata_view,
        boost::scoped_ptr<broadcaster_t<memcached_protocol_t> > *broadcaster,
        test_store_t<memcached_protocol_t> *,
        boost::scoped_ptr<listener_t<memcached_protocol_t> > *initial_listener)
{
    /* Set up a replier so the broadcaster can handle operations */
    EXPECT_FALSE((*initial_listener)->get_broadcaster_lost_signal()->is_pulsed());
    replier_t<memcached_protocol_t> replier(initial_listener->get());

    simple_directory_manager_t<boost::optional<replier_business_card_t<memcached_protocol_t> > >
        replier_directory_controller(
            cluster,
            boost::optional<replier_business_card_t<memcached_protocol_t> >(replier.get_business_card())
            );

    order_source_t order_source;

    /* Start sending operations to the broadcaster */
    std::map<std::string, std::string> inserter_state;
    test_inserter_t inserter(
        boost::bind(&write_to_broadcaster, broadcaster->get(), _1, _2, _3, _4),
        NULL,
        &order_source,
        &inserter_state);
    nap(100);

    /* Set up a second mirror */
    test_store_t<memcached_protocol_t> store2;
    memcached_protocol_t::region_t subregion(key_range_t::closed, store_key_t("a"), key_range_t::closed, store_key_t("z"));
    store_subview_t<memcached_protocol_t> substore(&store2.store, subregion);
    cond_t interruptor;
    listener_t<memcached_protocol_t> listener2(
        cluster->get_mailbox_manager(),
        broadcaster_metadata_view,
        branch_history_view,
        &substore,
        replier_directory_controller.get_root_view()->
            get_peer_view(cluster->get_connectivity_service()->get_me()),
        &interruptor);

    EXPECT_FALSE((*initial_listener)->get_broadcaster_lost_signal()->is_pulsed());
    EXPECT_FALSE(listener2.get_broadcaster_lost_signal()->is_pulsed());

    nap(100);

    /* Stop the inserter, then let any lingering writes finish */
    inserter.stop();
    /* Let any lingering writes finish */
    let_stuff_happen();

    for (std::map<std::string, std::string>::iterator it = inserter_state.begin();
            it != inserter_state.end(); it++) {
        get_query_t get;
        get.key = store_key_t(it->first);
        memcached_protocol_t::read_t read(get, time(NULL));
        memcached_protocol_t::read_response_t response =
            broadcaster->get()->read(read, order_source.check_in("unittest"));
        get_result_t get_result = boost::get<get_result_t>(response.result);
        EXPECT_TRUE(get_result.value.get() != NULL);
        EXPECT_EQ(it->second.size(), get_result.value->size());
        if (get_result.value->size() == (int)it->second.size()) {
            EXPECT_EQ(it->second, std::string(get_result.value->buf(), get_result.value->size()));
        }
    }
}
TEST(MemcachedBackfill, PartialBackfill) {
    run_in_thread_pool_with_broadcaster(&run_partial_backfill_test);
}

}   /* namespace unittest */