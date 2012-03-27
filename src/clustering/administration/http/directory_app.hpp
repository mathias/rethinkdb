#ifndef CLUSTERING_ADMINSTRATION_HTTP_DIRECTORY_APP_HPP_
#define CLUSTERING_ADMINSTRATION_HTTP_DIRECTORY_APP_HPP_

#include "clustering/administration/metadata.hpp"
#include "http/http.hpp"
#include "http/json/cJSON.hpp"
#include "rpc/directory/read_view.hpp"

#include <string>
#include <vector>

class directory_http_app_t : public http_app_t {
public:
    explicit directory_http_app_t(clone_ptr_t<directory_rview_t<cluster_directory_metadata_t> >& _directory_metadata);
    http_res_t handle(const http_req_t &);
private:
    cJSON *get_metadata_json(cluster_directory_metadata_t& metadata, const std::vector<std::string>& path) THROWS_ONLY(schema_mismatch_exc_t);

    clone_ptr_t<directory_rview_t<cluster_directory_metadata_t> > directory_metadata;
};

#endif