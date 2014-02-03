#ifndef __WOOKIE_SIMILARITY_HPP
#define __WOOKIE_SIMILARITY_HPP

#include "wookie/hash.hpp"
#include "wookie/parser.hpp"
#include "wookie/lexical_cast.hpp"
#include "wookie/timer.hpp"

#include <algorithm>
#include <fstream>
#include <list>
#include <sstream>
#include <vector>

#include <iconv.h>
#include <string.h>

#include <boost/locale.hpp>
#include <boost/program_options.hpp>

#include <msgpack.hpp>

namespace ioremap { namespace similarity {

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

struct ngram {
	ngram(int n, std::vector<long> &h) : n(n) {
		hashes.swap(h);
	}

	ngram() : n(0) {}
	ngram(int n) : n(n) {}
	ngram(const ngram &src) {
		n = src.n;
		hashes = src.hashes;
	}

	ngram(ngram &&src) {
		n = src.n;
		hashes.swap(src.hashes);
	}

	int n;
	std::vector<long> hashes;

	static ngram intersect(const ngram &f, const ngram &s) {
		ngram tmp(f.n);
		tmp.hashes.resize(f.hashes.size());

		auto inter = std::set_intersection(f.hashes.begin(), f.hashes.end(),
				s.hashes.begin(), s.hashes.end(), tmp.hashes.begin());
		tmp.hashes.resize(inter - tmp.hashes.begin());

		return tmp;
	}

	MSGPACK_DEFINE(n, hashes);
};

#define NGRAM_START	1
#define NGRAM_NUM	3


class document_parser {
	public:
		document_parser() : m_loc(m_gen("en_US.UTF8")) {
		}

		void feed(const char *path, const std::string &enc) {
			std::ifstream in(path);
			if (in.bad())
				return;

			std::ostringstream ss;

			ss << in.rdbuf();

			m_parser.parse(ss.str(), enc);
		}

		std::string text(void) const {
			return m_parser.text(" ");
		}

		void generate_ngrams(const std::string &text, std::vector<ngram> &ngrams) {
			for (int i = NGRAM_START; i <= NGRAM_START + NGRAM_NUM; ++i) {
				std::vector<long> hashes;

				generate(text, i, hashes);

				ngrams.emplace_back(i, hashes);
			}
		}


	private:
		wookie::parser m_parser;
		boost::locale::generator m_gen;
		std::locale m_loc;

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

					hashes.emplace_back(wookie::hash::murmur(ss.str(), 0));
					ngram.pop_front();
				}
			}

			std::set<long> tmp(hashes.begin(), hashes.end());
			hashes.assign(tmp.begin(), tmp.end());
		}
};

class document {
	public:
		document(int doc_id) : m_doc_id(doc_id) {
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
		std::vector<ngram> m_ngrams;
};

struct learn_element {
	learn_element() : valid(false) {
	}

	std::vector<int> doc_ids;
	std::string request;
	bool valid;

	std::vector<int> features;
};

}} // namespace ioremap::similarity

#endif /* __WOOKIE_SIMILARITY_HPP */