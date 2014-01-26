#include "wookie/parser.hpp"
#include "wookie/lexical_cast.hpp"

#include <algorithm>
#include <fstream>
#include <list>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <iconv.h>
#include <string.h>

#include <boost/locale.hpp>
#include <boost/program_options.hpp>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#include <dlib/svm.h>
#pragma GCC diagnostic pop

using namespace ioremap;

class charset_convert {
	public:
		charset_convert(const char *to, const char *from) {
			m_tmp.resize(128);

			m_iconv = iconv_open(to, from);
			if (m_iconv == (iconv_t)-1) {
				int err = errno;
				std::ostringstream ss;
				ss << "invalid conversion: " <<
					from << " -> " << to << " : " << strerror(err) << " [" << err << "]";
				throw std::runtime_error(ss.str());
			}
		}

		~charset_convert() {
			iconv_close(m_iconv);
		}

		void reset(void) {
			::iconv(m_iconv, NULL, NULL, NULL, NULL);
		}

		std::string convert(const std::string &in) {
			char *src = const_cast<char *>(&in[0]);
			size_t inleft = in.size();

			std::ostringstream out;

			while (inleft > 0) {
				char *dst = const_cast<char *>(&m_tmp[0]);
				size_t outleft = m_tmp.size();

				size_t size = ::iconv(m_iconv, &src, &inleft, &dst, &outleft);

				if (size == (size_t)-1) {
					if (errno == EINVAL)
						break;
					if (errno == E2BIG)
						continue;
					if (errno == EILSEQ) {
						src++;
						inleft--;
						continue;
					}
				}

				out.write(m_tmp.c_str(), m_tmp.size() - outleft);
			}

			return out.str();
		}

	private:
		iconv_t m_iconv;
		std::string m_tmp;
};

class document_parser {
	public:
		document_parser() : m_loc(m_gen("en_UR.UTF8")) {
		}

		void feed(const char *path) {
			std::ifstream in(path);
			if (in.bad())
				return;

			std::ostringstream ss;

			ss << in.rdbuf();

			m_parser.parse(ss.str(), "utf8");
		}

		std::string text(void) const {
			return m_parser.text(" ");
		}

		void generate(const std::string &text, int ngram_num, std::vector<long> &hashes) {
			namespace lb = boost::locale::boundary;
			lb::ssegment_index wmap(lb::word, text.begin(), text.end(), m_loc);
			wmap.rule(lb::word_any);

			std::list<std::string> ngram;

			for (auto it = wmap.begin(), e = wmap.end(); it != e; ++it) {
				std::string token = boost::locale::to_lower(it->str(), m_loc);

				ngram.push_back(token);

				if ((int)ngram.size() == ngram_num) {
					std::ostringstream ss;
					std::copy(ngram.begin(), ngram.end(), std::ostream_iterator<std::string>(ss, ""));

					hashes.emplace_back(hash(ss.str(), 0));
					ngram.pop_front();
				}
			}

			std::set<long> tmp(hashes.begin(), hashes.end());
			hashes.assign(tmp.begin(), tmp.end());
		}

	private:
		wookie::parser m_parser;
		boost::locale::generator m_gen;
		std::locale m_loc;

		// murmur hash
		long hash(const std::string &str, long seed) const {
			const uint64_t m = 0xc6a4a7935bd1e995LLU;
			const int r = 47;

			long h = seed ^ (str.size() * m);

			const uint64_t *data = (const uint64_t *)str.data();
			const uint64_t *end = data + (str.size() / 8);

			while (data != end) {
				uint64_t k = *data++;

				k *= m;
				k ^= k >> r;
				k *= m;

				h ^= k;
				h *= m;
			}

			const unsigned char *data2 = (const unsigned char *)data;

			switch (str.size() & 7) {
			case 7: h ^= (uint64_t)data2[6] << 48;
			case 6: h ^= (uint64_t)data2[5] << 40;
			case 5: h ^= (uint64_t)data2[4] << 32;
			case 4: h ^= (uint64_t)data2[3] << 24;
			case 3: h ^= (uint64_t)data2[2] << 16;
			case 2: h ^= (uint64_t)data2[1] << 8;
			case 1: h ^= (uint64_t)data2[0];
				h *= m;
			};

			h ^= h >> r;
			h *= m;
			h ^= h >> r;

			return h;
		}
};

struct ngram {
	ngram(std::vector<long> &h) {
		hashes.swap(h);
	}

	ngram(void) {}

	std::vector<long> hashes;
};

class document {
	public:
		document(int doc_id, const char *path) : m_doc_id(doc_id), m_path(path) {
		}

		const std::string &name(void) const {
			return m_path;
		}

		std::vector<ngram> &ngrams(void) {
			return m_ngrams;
		}

		const std::vector<ngram> &ngrams(void) const {
			return m_ngrams;
		}

		int id(void) const {
			return m_doc_id;
		}

	private:
		int m_doc_id;
		std::string m_path;
		std::vector<ngram> m_ngrams;
};

#define NGRAM_START	1
#define NGRAM_NUM	4

struct learn_element {
	learn_element() : valid(false) {
	}

	std::vector<int> doc_ids;
	std::vector<document> docs;
	std::string request;
	bool valid;

	std::vector<int> features;
};

class dlib_learner {
	public:
		dlib_learner() {}

		void add_sample(const learn_element &le, int label) {
			if (!le.valid)
				return;

			sample_type s;
			s.set_size(le.features.size());

			std::ostringstream ss;
			ss << le.request << ": ";
			for (size_t i = 0; i < le.features.size(); ++i) {
				s(i) = le.features[i];
				ss << le.features[i] << " ";
			}

			ss << ": " << label << std::endl;
			std::cout << ss.str();

			m_samples.push_back(s);
			m_labels.push_back(label);
		}

	private:
		typedef dlib::matrix<double, 0, 1> sample_type;
		typedef dlib::radial_basis_kernel<sample_type> kernel_type;

		std::vector<sample_type> m_samples;
		std::vector<double> m_labels;
};

class learner {
	public:
		learner(const std::string &input, const std::string &learn_file) : m_input(input) {
			srand(time(NULL));

			std::ifstream in(learn_file.c_str());

			std::string line;
			int line_num = 0;
			while (std::getline(in, line)) {
				if (!in.good())
					break;

				line_num++;

				int doc[2];

				int num = sscanf(line.c_str(), "%d\t%d\t", &doc[0], &doc[1]);
				if (num != 2) {
					fprintf(stderr, "failed to parse string: %d, tokens found: %d\n", line_num, num);
					continue;
				}

				const char *pos = strrchr(line.c_str(), '\t');
				if (!pos) {
					fprintf(stderr, "could not find last tab delimiter\n");
					continue;
				}

				pos++;
				if (pos && *pos) {
					learn_element le;

					le.doc_ids = std::vector<int>(doc, doc+2);
					le.request.assign(pos);

					m_elements.emplace_back(std::move(le));
				}
			}

			printf("pairs loaded: %zd\n", m_elements.size());
			m_negative_elements.resize(m_elements.size());

			add_documents(8);

			dlib_learner dl;

			for (size_t i = 0; i < m_elements.size(); ++i) {
				dl.add_sample(m_elements[i], +1);
				dl.add_sample(m_negative_elements[i], -1);
			}
		}

	private:
		std::string m_input;
		std::vector<learn_element> m_elements;
		std::vector<learn_element> m_negative_elements;

		struct doc_thread {
			int id;
			int step;
		};

		void generate_ngrams(document_parser &parser, const std::string &text, std::vector<ngram> &ngrams) {
			for (int i = NGRAM_START; i <= NGRAM_START + NGRAM_NUM; ++i) {
				std::vector<long> hashes;

				parser.generate(text, i, hashes);

				ngrams.emplace_back(hashes);
			}
		}

		void load_documents(struct doc_thread &dth) {
			document_parser parser;

			for (size_t i = dth.id; i < m_elements.size(); i += dth.step) {
				learn_element &le = m_elements[i];

				std::vector<ngram> req_ngrams; 
				generate_ngrams(parser, le.request, req_ngrams);

				for (auto doc_id : le.doc_ids) {
					std::string file = m_input + lexical_cast(doc_id) + ".html";
					try {
						parser.feed(file.c_str());
						std::string text = parser.text();

						document doc(doc_id, file.c_str());
						generate_ngrams(parser, text, doc.ngrams());

						le.docs.emplace_back(doc);
					} catch (const std::exception &e) {
						std::cerr << file << ": caught exception: " << e.what() << std::endl;
						break;
					}
				}

				generate_features(le, req_ngrams);
				generate_negative_element(le, i, m_negative_elements[i], req_ngrams);
			}
		}

		ngram intersect(const ngram &f, const ngram &s) {
			ngram tmp;
			tmp.hashes.resize(f.hashes.size());

			auto inter = std::set_intersection(f.hashes.begin(), f.hashes.end(),
					s.hashes.begin(), s.hashes.end(), tmp.hashes.begin());
			tmp.hashes.resize(inter - tmp.hashes.begin());

			return tmp;
		}

		void generate_features(learn_element &le, const std::vector<ngram> &req_ngrams) {
			const std::vector<ngram> &f = le.docs[0].ngrams();
			const std::vector<ngram> &s = le.docs[1].ngrams();

			std::ostringstream ss;

			for (size_t i = 0; i < req_ngrams.size(); ++i) {
				ngram out = intersect(f[i], s[i]);
				le.features.push_back(out.hashes.size());

				ngram req_out = intersect(req_ngrams[i], out);
				le.features.push_back(req_out.hashes.size());

			}

			le.features.push_back(req_ngrams[0].hashes.size());
			le.valid = true;
		}

		void add_documents(int cpunum) {
			std::vector<std::thread> threads;

			for (int i = 0; i < cpunum; ++i) {
				struct doc_thread dth;

				dth.id = i;
				dth.step = cpunum;

				threads.emplace_back(std::bind(&learner::load_documents, this, dth));
			}

			for (int i = 0; i < cpunum; ++i) {
				threads[i].join();
			}
		}

		void generate_negative_element(learn_element &le, int position,
				learn_element &negative, const std::vector<ngram> &req_ngrams) {

			if (!le.valid)
				return;

			int doc_id = le.doc_ids[0];

			negative.doc_ids.push_back(doc_id);
			negative.docs.push_back(le.docs[0]);

			negative.request = le.request;

			if (!position)
				position = 1;

			int total = 0;
			int total_max = 10;
			while (++total < total_max) {
				int pos = rand() % position;
				le = m_elements[pos];

				if (!le.valid)
					continue;

				if ((doc_id != le.doc_ids[0]) && (doc_id != le.doc_ids[1])) {
					negative.doc_ids.push_back(le.doc_ids[0]);
					negative.docs.push_back(le.docs[0]);
					break;
				}
			}

			if (total >= total_max)
				return;

			generate_features(negative, req_ngrams);
		}
};

int main(int argc, char *argv[])
{
	namespace bpo = boost::program_options;

	bpo::options_description generic("Similarity options");

	std::string mode, input, learn_file;
	generic.add_options()
		("help", "This help message")
		("input", bpo::value<std::string>(&input), "Input directory")
		("learn", bpo::value<std::string>(&learn_file), "Learning data file")
		("mode", bpo::value<std::string>(&mode)->default_value("learn"), "Processing mode: learn/check")
		;

	bpo::variables_map vm;

	try {
		bpo::store(bpo::parse_command_line(argc, argv, generic), vm);
		bpo::notify(vm);
	} catch (const std::exception &e) {
		std::cerr << "Invalid options: " << e.what() << "\n" << generic << std::endl;
		return -1;
	}

	if (!vm.count("input")) {
		std::cerr << "No input directory\n" << generic << std::endl;
		return -1;
	}

	if ((mode == "learn") && !vm.count("learn")) {
		std::cerr << "Learning mode requires file with learning data\n" << generic << std::endl;
		return -1;
	}

	xmlInitParser();

	if (mode == "learn") {
		learner l(input, learn_file);
		return -1;
	}
#if 0
	std::vector<document> docs;

	for (auto f : files) {
		try {
			p.feed(argv[i], 4);
			if (p.hashes().size() > 0)
				docs.emplace_back(argv[i], p.hashes());

#if 0
			std::cout << "================================" << std::endl;
			std::cout << argv[i] << ": hashes: " << p.hashes().size() << std::endl;
			std::ostringstream ss;
			std::vector<std::string> tokens = p.tokens();

			std::copy(tokens.begin(), tokens.end(), std::ostream_iterator<std::string>(ss, " "));
			std::cout << ss.str() << std::endl;
#endif
		} catch (const std::exception &e) {
			std::cerr << argv[i] << ": caught exception: " << e.what() << std::endl;
		}
	}


#endif
}
