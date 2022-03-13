#pragma once
#include <vector>
#include <algorithm>
#include <Windows.h>


template<typename T>
inline std::vector<float> linspace(T start_in, T end_in, int num_in)
{
	std::vector<float> linspaced;
	float start = static_cast<float>(start_in);
	float end = static_cast<float>(end_in);
	float num = static_cast<float>(num_in);
	if (num == 0) { return linspaced; }
	if (num == 1)
	{
		linspaced.push_back(start);
		return linspaced;
	}
	float delta = (end - start) / (num - 1);
	for (int i = 0; i < num - 1; ++i)
	{
		linspaced.push_back(start + delta * i);
	}
	linspaced.push_back(end);
	return linspaced;
}


class WavenumberInterpolationPlan
{

public:
	int aline_size;
	double interpdk;
	std::vector<std::vector<int>> interp_map;  // Map of nearest neighbors
	std::vector<float> linear_in_lambda;  // Linear wavelength space
	std::vector<float> linear_in_k;  // Linear wavenumber space points to interpolate
	float d_lam;

	WavenumberInterpolationPlan() {}

	WavenumberInterpolationPlan(int aline_size, double interpdk)
	{
		linear_in_lambda = linspace(1 - (interpdk / 2), 1 + (interpdk / 2), aline_size);
		for (int i = 0; i < aline_size; i++)
		{
			linear_in_lambda[i] = 1 / linear_in_lambda[i];
		}
		float min_lam = *std::min_element(linear_in_lambda.begin(), linear_in_lambda.end());
		float max_lam = *std::max_element(linear_in_lambda.begin(), linear_in_lambda.end());
		linear_in_k = linspace(min_lam, max_lam, aline_size);

		d_lam = linear_in_lambda[1] - linear_in_lambda[0];

		interp_map = std::vector<std::vector<int>>(2);
		interp_map[0] = std::vector<int>(aline_size);  // Left nearest-neighbor
		interp_map[1] = std::vector<int>(aline_size);  // Right nearest-neighbor

		//(Naively, but only once) find nearest upper and lower indices for linear interpolation
		for (int i = 0; i < aline_size; i++)  // For each k-linearized interpolation point
		{
			// Calculate distance vector
			std::vector<float> distances = std::vector<float>(aline_size);
			for (int j = 0; j < aline_size; j++)  // For each linear-in-wavelength point
			{
				distances[j] = std::abs(linear_in_lambda[j] - linear_in_k[i]);
			}
			auto nn = std::min_element(distances.begin(), distances.end()) - distances.begin();  // Find closest point
			if (nn == 0)
			{
				interp_map[0][i] = 0;
				interp_map[1][i] = 0;
			}
			else if (nn == aline_size - 1)
			{
				interp_map[0][i] = aline_size - 1;
				interp_map[1][i] = aline_size - 1;
			}
			else if (linear_in_lambda[nn] >= linear_in_k[nn])
			{
				interp_map[0][i] = nn - 1;
				interp_map[1][i] = nn;
			}
			else if (linear_in_lambda[nn] < linear_in_k[nn])
			{
				interp_map[0][i] = nn;
				interp_map[1][i] = nn + 1;
			}
		}
	}

};


void interpdk_execute(WavenumberInterpolationPlan plan, int number_of_alines, uint16_t* raw_src, float* interpolated)
{
	float interp_y0;
	float interp_y1;
	float interp_dy;
	float interp_dx;

	// k-linearization and DC subtraction
	for (int i = 0; i < number_of_alines; i++)
	{
		raw_src[plan.aline_size * i] = 0;
		for (int j = 0; j < plan.aline_size; j++)  // For each element of each A-line
		{
			interp_y0 = raw_src[plan.aline_size * i + plan.interp_map[0][j]];
			interp_y1 = raw_src[plan.aline_size * i + plan.interp_map[1][j]];
			if (plan.interp_map[0][i] == plan.interp_map[1][i])
			{
				interpolated[plan.aline_size * i + j] = interp_y0;
			}
			else
			{
				interp_dy = interp_y1 - interp_y0;
				interp_dx = plan.linear_in_k[j] - plan.linear_in_lambda[plan.interp_map[0][j]];
				interpolated[plan.aline_size * i + j] = interp_y0 + interp_dx * (interp_dy / plan.d_lam);
			}
		}
	}
}
