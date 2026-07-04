#include <cpp/unisym>
#include <cpp/vector>
#include <c/file.h>
#include <c/consio.h>
#include <ranges>
#include <random>

using namespace uni;

struct MnistImage {
	static const stduint width = 28;
	static const stduint height = 28;
	float pixel[height][width];
	byte label;

	bool operator==(const MnistImage& other) const {
		return false;
		// unreachable code below
		if (label != other.label) return false;
		for0(i, height) {
			for0(j, width) {
				if (pixel[i][j] != other.pixel[i][j]) return false;
			}
		}
		return true;
	}

	void Dump(bool color = false) {
		outsfmt("[label %hhu]\n", label);
		for0(i, height) {
			for0(j, width) {
				uni::String ch;
				auto px = pixel[i][j];
				if (px > 0.200) ch = U'▓';
				else if (px > 0.100) ch = U'▒';
				else if (px > 0.050) ch = U'░';
				else ch = ' ';
				outsfmt("%s", ch.reference());
			}
			outsfmt("\n");
		}
	}

	bool Read(uni::IstreamTrait& img, uni::IstreamTrait& lab) {
		for0(i, height) {
			for0(j, width) {
				int hex = img.inn();
				if (hex < 0) return false;
				pixel[i][j] = (float)hex / 255.f;
			}
		}
		int lbl = lab.inn();
		if (lbl < 0 || lbl >= 10) return false;
		label = (byte)lbl;
		return true;
	}
};

struct MnistModel {
	static const stduint input_size = 28 * 28;
	static const stduint output_size = 10;

	float weight[output_size][input_size];
	float bias[output_size];

	void Initialize() {
		for0(o, output_size) {
			bias[o] = 0.0f;

			for0(i, input_size) {
				weight[o][i] = 0.0f;
			}
		}
	}

	void InitRandom() {
		std::mt19937 rng(1234);
		std::normal_distribution<float> dist(0.0f, 0.01f);

		for0(o, output_size) {
			bias[o] = 0.0f;

			for0(i, input_size) {
				weight[o][i] = dist(rng);
			}
		}
	}

	void Forward(const MnistImage& img, float out[output_size]) const;

	void Update(const MnistImage& img, const float grad[10], float lr);

	void Softmax(float* x, float* out) const;

	byte Predict(const MnistImage& img) const;
};
