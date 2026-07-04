#include "mnist.hpp"

void MnistModel::Forward(const MnistImage& img, float out[output_size]) const {
	for0(o, output_size) {
		float sum = bias[o];

		for0(y, MnistImage::height) {
			for0(x, MnistImage::width) {
				stduint i = y * MnistImage::width + x;
				sum += weight[o][i] * img.pixel[y][x];
			}
		}

		out[o] = sum;
	}
}



void MnistModel::Update(const MnistImage& img, const float grad[10], float lr) {
	for0(o, output_size) {
		bias[o] -= lr * grad[o];

		for0(i, input_size) {
			float x = img.pixel[i / 28][i % 28];
			weight[o][i] -= lr * grad[o] * x;
		}
	}
}

void MnistModel::Softmax(float* x, float* out) const {
	float maxv = x[0];

	for0(i, 10) {
		if (x[i] > maxv) maxv = x[i];
	}

	float sum = 0.0f;

	for0(i, 10) {
		out[i] = std::exp(x[i] - maxv);
		sum += out[i];
	}

	for0(i, 10) {
		out[i] /= sum;
	}
}

byte MnistModel::Predict(const MnistImage& img) const {
	float score[output_size];
	Forward(img, score);

	byte best = 0;
	float best_score = score[0];

	for (byte i = 1; i < output_size; ++i) {
		if (score[i] > best_score) {
			best_score = score[i];
			best = i;
		}
	}

	return best;
}
