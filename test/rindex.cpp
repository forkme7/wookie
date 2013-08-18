#include "wookie/storage.hpp"
#include "wookie/operators.hpp"
#include "wookie/url.hpp"

#include "wookie/engine.hpp"
#include "wookie/index_data.hpp"
#include "wookie/basic_elliptics_splitter.hpp"

#include <boost/program_options.hpp>

#include "thevoid/elliptics/jsonvalue.hpp"
#include "thevoid/elliptics/index.hpp"

using namespace ioremap;
using namespace ioremap::wookie;

struct rindex_processor
{
	wookie::engine &engine;
	std::string base;
	bool fallback;

	basic_elliptics_splitter m_splitter;

	rindex_processor(wookie::engine &engine, const std::string &base, bool fallback)
		: engine(engine), base(base), fallback(fallback) {
	}

	void process(const std::string &url, const std::string &content, const dnet_time &ts, const std::string &base_index) {
		std::vector<std::string> ids;
		std::vector<elliptics::data_pointer> objs;

		m_splitter.process(url, content, ts, base_index, ids, objs);

		if (ids.size()) {
			std::cout << "Rindex update ... url: " << url << ": indexes: " << ids.size() << std::endl;
			engine.get_storage()->create_session().set_indexes(url, ids, objs).wait();
		}
	}

	void process_text(const ioremap::swarm::network_reply &reply, document_type) {
		struct dnet_time ts;
		dnet_current_time(&ts);

		parser p;
		if (!fallback)
			p.parse(reply.get_data());

		try {
			process(reply.get_url(), p.text(), ts, base + ".collection");
		} catch (const std::exception &e) {
			std::cerr << reply.get_url() << ": index processing exception: " << e.what() << std::endl;
			engine.download(reply.get_request().get_url());
		}
	}

	static process_functor create(wookie::engine &engine, const std::string &url, bool fallback) {
		ioremap::swarm::network_url base_url;
		std::string base;

		if (!base_url.set_base(url))
			ioremap::elliptics::throw_error(-EINVAL, "Invalid URL '%s': set-base failed", url.c_str());

		base = base_url.host();
		if (base.empty())
			ioremap::elliptics::throw_error(-EINVAL, "Invalid URL '%s': base is empty", url.c_str());

		return std::bind(&rindex_processor::process_text,
			std::make_shared<rindex_processor>(engine, base, fallback),
			std::placeholders::_1,
			std::placeholders::_2);
	}
};

int main(int argc, char *argv[])
{
	using namespace boost::program_options;

	std::string find;
	std::string url;
	variables_map vm;
	wookie::engine engine;

	engine.add_options("RIndex options")
		("find", value<std::string>(&find), "Find pages containing all tokens (space separated, supports quotes for exact match)")
		("url", value<std::string>(&url), "Url to download")
		("json", "Output json with pages content which contain requested tokens")
	;

	try {
		int err = engine.parse_command_line(argc, argv, vm);
		if (err < 0)
			return err;
	} catch (const std::exception &e) {
		std::cerr << "Command line parsing failed: " << e.what() << std::endl;
		engine.show_help_message(std::cerr);
		return -1;
	}

	if (!vm.count("find") && !vm.count("url")) {
		std::cerr << "You must provide either URL or FIND option" << std::endl;
		engine.show_help_message(std::cerr);
		return -1;
	}

	try {
		if (find.size()) {
			operators op(*engine.get_storage());
			auto find_result = op.find(find);

			const std::vector<dnet_raw_id> &ids = find_result->results_array();

			std::cout << "Found " << ids.size() << " documents for request: " << find << std::endl;
			if (!ids.size())
				return -ENOENT;

			if (vm.count("json") == 0) {
				char tmp_str[DNET_ID_SIZE * 2 + 1];
				for (auto && r : ids) {
					std::cout << dnet_dump_id_len_raw(r.id, DNET_ID_SIZE, tmp_str) << std::endl;
				}
			} else {
				elliptics::session sess = engine.get_storage()->create_session();

				sess.set_ioflags(DNET_IO_FLAGS_CACHE);

				std::vector<elliptics::key> keys;
				std::copy(ids.begin(), ids.end(), std::back_inserter(keys));
				auto read_results = sess.bulk_read(keys).get();

				struct document_unpacker {
					const std::string operator () (const elliptics::data_pointer &data) {
						document doc = storage::unpack_document(data);
						return doc.key;
					}
				};

				struct index_unpacker {
					const std::string operator () (const elliptics::data_pointer &data) {
						index_data idata(data);

						std::stringstream result;
						std::copy(idata.pos.begin(), idata.pos.end(), std::ostream_iterator<int>(result, " "));
						return result.str();
					}
				};

				thevoid::elliptics::JsonValue result_object;
				thevoid::elliptics::index::find_serializer::pack_indexes_json(result_object, read_results, document_unpacker(),
						find_result->results_find_indexes_array(), index_unpacker(), find_result->index_map());

				std::cout << result_object.ToString() << std::endl;
			}
		} else {
			engine.add_filter(create_text_filter());
			engine.add_url_filter(create_domain_filter(url));
			engine.add_parser(create_href_parser());
			engine.add_processor(rindex_processor::create(engine, url, false));
			engine.add_fallback_processor(rindex_processor::create(engine, url, true));

			engine.download(url);

			return engine.run();
		}
	} catch (const std::exception &e) {
		std::cerr << "main thread exception: " << e.what() << std::endl;
	}
	return 0;

}
