#include "mnist.hpp"

void MnistModel::Forward(const MnistImage& img, float out[output_size]) const {

	float h[hidden_size];

	// layer1
	for0(i, hidden_size) {
		float sum = b1[i];
		for0(j, input_size) {
			sum += W1[i][j] * img.pixel[j / 28][j % 28];
		}
		h[i] = std::max(0.0f, sum); // ReLU
	}

	// layer2
	for0(o, output_size) {
		float sum = b2[o];
		for0(i, hidden_size) {
			sum += W2[o][i] * h[i];
		}
		out[o] = sum;
	}
}



void MnistModel::Update(const MnistImage& img, const float grad[10], float lr) {
	// 网络：
	//   x -> z1 -> h -> score -> loss
	// 正向：
	//   z1 = W1*x + b1
	//   h = ReLU(z1)
	//   score = W2*h + b2
	// 反向：
	//   g2 = dLoss/dScore = grad = prob - one_hot(label)
	//   g1 = dLoss/dZ1 = (W2^T * g2) * ReLU'(z1)
	// 更新：
	//   W2 -= lr * (g2 * h)
	//   b2 -= lr * g2
	//   W1 -= lr * (g1 * x)
	//   b1 -= lr * g1
	// 注意：先算 g1，再更新 W2；g1 必须使用旧 W2。
	float z1[hidden_size];
	float h[hidden_size];

	// 1. 第一层 forward
	// z1[i] = b1[i] + Σ_j W1[i][j] * x[j]
	// h[i] = max(0, z1[i])
	for0(i, hidden_size) {
		float sum = b1[i];
		for0(j, input_size) {
			float x = img.pixel[j / 28][j % 28];
			sum += W1[i][j] * x;
		}
		z1[i] = sum;
		h[i] = std::max(0.0f, sum);
	}

	// 2. 输出层梯度
	// g2[o] = dLoss/dScore[o] = prob[o] - one_hot(label)[o]
	float g2[output_size];
	for0(o, output_size) {
		g2[o] = grad[o];
	}

	// 3. 隐藏层梯度
	// dh[i] = dLoss/dh[i] = Σ_o W2[o][i] * g2[o]
	// g1[i] = dLoss/dz1[i] = dh[i] * ReLU'(z1[i])
	// ReLU'(z1) = 1 if z1 > 0 else 0
	float g1[hidden_size] = { 0 };
	for0(i, hidden_size) {
		float dh = 0.0f;
		for0(o, output_size) {
			dh += W2[o][i] * g2[o];
		}
		g1[i] = (z1[i] > 0.0f) ? dh : 0.0f;
	}

	// 4. 更新第二层
	// dLoss/db2[o] = g2[o]
	// dLoss/dW2[o][i] = g2[o] * h[i]
	for0(o, output_size) {
		b2[o] -= lr * g2[o];
		for0(i, hidden_size) {
			W2[o][i] -= lr * g2[o] * h[i];
		}
	}

	// 5. 更新第一层
	// dLoss/db1[i] = g1[i]
	// dLoss/dW1[i][j] = g1[i] * x[j]
	for0(i, hidden_size) {
		b1[i] -= lr * g1[i];
		for0(j, input_size) {
			float x = img.pixel[j / 28][j % 28];
			W1[i][j] -= lr * g1[i] * x;
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
