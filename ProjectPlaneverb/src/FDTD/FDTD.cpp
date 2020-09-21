#include <FDTD\Grid.h>
#include <Planeverb.h>
#include <PvDefinitions.h>

#include <Context\PvContext.h>

#include <DSP\Analyzer.h>
#include <Emissions\EmissionManager.h>
#include <Util/ScopedTimer.h>
#include <omp.h>
#include <iostream>

namespace Planeverb
{
#pragma region ClientInterface
	PlaneverbOutput GetOutput(EmissionID emitter)
	{
		PlaneverbOutput out;
		std::memset(&out, 0, sizeof(out));
		auto* context = GetContext();

		// case module hasn't been created yet
		if(!context)
		{
			out.occlusion = PV_INVALID_DRY_GAIN;
			return out;
		}

		auto* analyzer = context->GetAnalyzer();
		auto* emissions = context->GetEmissionManager();
		const auto* emitterPos = emissions->GetEmitter(emitter);

		// case emitter is invalid
		if (!emitterPos)
		{
			out.occlusion = PV_INVALID_DRY_GAIN;
			return out;
		}

		auto* result = analyzer->GetResponseResult(*emitterPos);

		// case invalid emitter position
		if (!result)
		{
			out.occlusion = 1.f;
			return out;
		}

		// copy over values
		out.occlusion = (float)result->occlusion;
        out.wetGain = (float)result->wetGain;
		out.lowpass = (float)result->lowpassIntensity;
		out.rt60 = (float)result->rt60;
		out.direction = result->direction;
		out.sourceDirectivity = result->sourceDirectivity;

		return out;
	}

	std::pair<const Cell*, unsigned> GetImpulseResponse(const vec3& position)
	{
		const auto& globals = Context::GetGlobals();
		Grid* grid = GetContext()->GetGrid();
		vec2 worldPosition =
		{
			position.x,
			position.z
		};
		return std::make_pair(grid->GetResponse(worldPosition), globals.responseSampleLength);
	}

#pragma endregion
	
	Cell* Grid::GetResponse(const vec2& worldSpace)
	{
		const auto& globals = Context::GetGlobals();
		int gridx, gridy;
		WorldToGrid(worldSpace, gridx, gridy);
		int index = INDEX2(gridx, gridy, (int)globals.gridSize.y + 1);
		return m_pulseResponse[index].data();
	}
	
	// process FDTD
	void Grid::GenerateResponseCPU(const vec3 &listener)
	{
		const auto& globals = Context::GetGlobals();

		// determine pressure and velocity update constants
		const Real Courant = PV_C * globals.simulationDT / globals.gridDX;

		// grid constants
		const int gridx = (int)globals.gridSize.x;
		const int gridy = (int)globals.gridSize.y;
		const vec2 dim = globals.gridSize;
		const vec2 incdim(dim.x + 1, dim.y + 1);
		int listenerPosX;
		int listenerPosY;
		if (globals.config.gridCenteringType == pv_StaticCentering)
		{
			WorldToGrid({ listener.x, listener.z }, listenerPosX, listenerPosY);
		}
		else
		{
			listenerPosX = gridx / 2;
			listenerPosY = gridy / 2;
		}
		const int listenerPos = INDEX2(listenerPosX, listenerPosY, gridy + 1);
		const int responseLength = globals.responseSampleLength;
		int loopSize = (int)(incdim.x) * (int)(incdim.y);

		// thread usage
		if (globals.config.maxThreadUsage == 0)
			omp_set_num_threads(omp_get_max_threads());
		else
			omp_set_num_threads(globals.config.maxThreadUsage);

		// RESET all pressure and velocity, but not B fields (can't use memset)
		{
			Cell* resetPtr = m_grid;
            const int N = loopSize;
			for (int i = 0; i < N; ++i, ++resetPtr)
			{
				resetPtr->pr = 0.f;
				resetPtr->vx = 0.f;
				resetPtr->vy = 0.f;
			}
		}

		// Time-stepped FDTD simulation
		for (int t = 0; t < responseLength; ++t)
		{
			// process pressure grid
			{
                const int N = loopSize;
                for (int i = 0; i < N; ++i)
				{
					Cell& thisCell = m_grid[i];
					int B = (int)thisCell.b;
					Real beta = (Real)B;
					//TODO: Check outside bounds access on ends?
					// [i + 1, j]
					const Cell& nextCellX = m_grid[i + gridy + 1];	
					// [i, j + 1]
					const Cell& nextCellY = m_grid[i + 1];

					const auto divergence = ((nextCellX.vx - thisCell.vx) + (nextCellY.vy - thisCell.vy));
					thisCell.pr = beta * (thisCell.pr - Courant * divergence);
				}
			}

			// process x component of particle velocity
			{
				// eq to for(1 to sizex) for(0 to sizey)
				for (int i = gridy + 1; i < loopSize; ++i)
				{
					// [i - 1, j]
					auto in = (i - gridy - 1);
					const Cell& prevCell = m_grid[in];
					Real beta_n = (Real)prevCell.b;
					Real Rn = prevCell.absorption; 
					Real Yn = (1.f - Rn) / (1.f + Rn);

					// [i, j]
					Cell& thisCell = m_grid[i];											
					int B = (int)thisCell.b;
					Real beta = (Real)B;
					Real R = thisCell.absorption;
					Real Y = (1.f - R) / (1.f + R);

					const Real gradient_x = (thisCell.pr - prevCell.pr);
					const Real airCellUpdate = thisCell.vx - Courant * gradient_x;

					const Real Y_boundary = beta * Yn + beta_n * Y;
					const Real wallCellUpdate = Y_boundary * (prevCell.pr * beta_n + thisCell.pr * beta);

					thisCell.vx = beta*beta_n * airCellUpdate + (beta_n - beta) * wallCellUpdate;
				}
			}

			// process y component of particle velocity
			{
				// eq to for(0 to sizex) for(1 to sizey)
				for (int i = 1; i < loopSize; ++i)
				{
					// [i, j - 1]
					const auto in = i - 1;
					const Cell& prevCell = m_grid[in];
					Real beta_n = (Real)prevCell.b;
					Real Rn = prevCell.absorption;
					Real Yn = (1.f - Rn) / (1.f + Rn);

					// [i, j]
					Cell& thisCell = m_grid[i];											
					int B = thisCell.b;
					Real beta = (Real)B;
					Real R = thisCell.absorption;
					Real Y = (1.f - R) / (1.f + R);
	
					const Real gradient_y = (thisCell.pr - prevCell.pr);
					const Real airCellUpdate = thisCell.vy - Courant * gradient_y;

					const Real Y_boundary = beta * Yn + beta_n * Y;
					const Real wallCellUpdate = Y_boundary * (prevCell.pr * beta_n + thisCell.pr * beta);

					thisCell.vy = beta * beta_n * airCellUpdate + (beta_n - beta) * wallCellUpdate;
				}
			}

			// process absorption top/bottom
			{
				for (int i = 0; i < gridy; ++i)
				{
					int index1 = i;
					int index2 = gridx * (gridy + 1) + i;

					m_grid[index1].vx = -m_grid[index1].pr;
					m_grid[index2].vx = m_grid[index2 - gridy - 1].pr;
				}
			}

			// process absorption left/right
			{
				for (int i = 0; i < gridx; ++i)
				{
					int index1 = i * (gridy + 1);
					int index2 = i * (gridy + 1) + gridy;

					m_grid[index1].vy = -m_grid[index1].pr;
					m_grid[index2].vy = m_grid[index2 - 1].pr;
				}
			}

			// add results to the response cube
			{
				for (int i = 0; i < loopSize; ++i)
				{
					m_pulseResponse[i][t] = m_grid[i];
				}
			}

			// add pulse to listener position pressure field
			m_grid[listenerPos].pr += m_pulse[t];
		}
	}

	void Grid::GenerateResponseGPU(const vec3& listener)
	{
		// not currently supported
		throw pv_InvalidConfig;
	}

	void Grid::GenerateResponse(const vec3& listener)
	{
		if (Context::GetGlobals().config.threadExecutionType == PlaneverbExecutionType::pv_CPU)
		{
			GenerateResponseCPU(listener);
		}
		else
		{
			GenerateResponseGPU(listener);
		}
	}
} // namespace Planeverb
