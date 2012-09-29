/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2012 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <mitsuba/bidir/util.h>
#include <mitsuba/core/fstream.h>
#include "mlt_proc.h"

MTS_NAMESPACE_BEGIN

/**
 * \order{10}
 * Veach-style Metropolis Light Transport implementation with support for
 * bidirectional mutations, lens perturbations, caustic perturbations and 
 * multi-chain perturbations. Several optimizations are also implemented, 
 * namely separate direct illumination, two-stage MLT, 
 * and importance sampling of mutation strategies. For details, see the 
 * respective parameter descriptions.
 */
class MLT : public Integrator {
public:
	MLT(const Properties &props) : Integrator(props) {
		/* Longest visualized path length (<tt>-1</tt>=infinite). 
		   A value of <tt>1</tt> will visualize only directly visible light 
		   sources. <tt>2</tt> will lead to single-bounce (direct-only) 
		   illumination, and so on. */
		m_config.maxDepth = props.getInteger("maxDepth", -1);

		/* This setting can be very useful to reduce noise in dark regions
		   of the image: it activates two-stage MLT, where a nested MLT renderer
		   first creates a tiny version of the output image. In a second pass, 
		   the full version is then rendered, while making use of information 
		   about the image-space luminance distribution found in the first
		   pass. Two-stage MLT is very useful in making the noise characteristics
		   more uniform over time image -- specifically, since MLT tends to get
		   stuck in very bright regions at the cost of the remainder of the image.*/
		m_config.twoStage = props.getBoolean("twoStage", false);

		/* When running two-stage MLT, this parameter influences the size
		   of the downsampled image created in the first pass (i.e. setting this
		   to 16 means that the horizontal/vertical resolution will be 16 times
		   lower). When the two-stage process introduces noisy halos around
		   very bright image regions, it might might be good to reduce this 
		   parameter to 4 or even 1. Generally though, it should be safe to leave
		   it unchanged. */
		m_config.firstStageSizeReduction = props.getInteger("firstStageSizeReduction", 16);

		/* Used internally to let the nested rendering process of a
		   two-stage MLT approach know that it is running the first stage */
		m_config.firstStage= props.getBoolean("firstStage", false);

		/* Number of samples used to estimate the total luminance 
		   received by the scene's sensor */
		m_config.luminanceSamples = props.getInteger("luminanceSamples", 100000);

		/* This parameter can be used to specify the samples per pixel used to 
		   render the direct component. Should be a power of two (otherwise, it will
		   be rounded to the next one). When set to zero or less, the
		   direct illumination component will be hidden, which is useful
		   for analyzing the component rendered by MLT. When set to -1, 
		   MLT will handle direct illumination as well */
		m_config.directSamples = props.getInteger("directSamples", 16);
		m_config.separateDirect = m_config.directSamples >= 0;

		/* Specifies the number of parallel work units required for
		   multithreaded and network rendering. When set to <tt>-1</tt>, the 
		   amount will default to four times the number of cores. Note that
		   every additional work unit entails a significant amount of 
		   communication overhead (a full-sized floating put image must be 
		   transmitted), hence it is important to set this value as low as 
		   possible, while ensuring that there are enough units to keep all 
		   workers busy. */
		m_config.workUnits = props.getInteger("workUnits", -1);
	
		/* Selectively enable/disable the bidirectional mutation */
		m_config.bidirectionalMutation = props.getBoolean("bidirectionalMutation", true);

		/* Selectively enable/disable the lens perturbation */
		m_config.lensPerturbation = props.getBoolean("lensPerturbation", false);

		/* Selectively enable/disable the caustic perturbation */
		m_config.causticPerturbation = props.getBoolean("causticPerturbation", false);

		/* Selectively enable/disable the multi-chain perturbation */
		m_config.multiChainPerturbation = props.getBoolean("multiChainPerturbation", false);

		/* Selectively enable/disable the manifold perturbation */ 
		m_config.manifoldPerturbation = props.getBoolean("manifoldPerturbation", false);
		m_config.probFactor = props.getFloat("probFactor", 50);
		m_config.timeout = props.getInteger("timeout", 0);
	}

	/// Unserialize from a binary data stream
	MLT(Stream *stream, InstanceManager *manager)
	 : Integrator(stream, manager) {
		m_config = MLTConfiguration(stream);
	}

	virtual ~MLT() { }

	void serialize(Stream *stream, InstanceManager *manager) const {
		Integrator::serialize(stream, manager);
		m_config.serialize(stream);
	}

	bool preprocess(const Scene *scene, RenderQueue *queue, 
			const RenderJob *job, int sceneResID, int sensorResID,
			int samplerResID) {
		Integrator::preprocess(scene, queue, job, sceneResID,
				sensorResID, samplerResID);

		if (scene->getSubsurfaceIntegrators().size() > 0)
			Log(EError, "Subsurface integrators are not supported by MLT!");

		if (scene->getSampler()->getClass()->getName() != "IndependentSampler")
			Log(EError, "Metropolis light transport requires the independent sampler");

		return true;
	}

	void cancel() {
		ref<RenderJob> nested = m_nestedJob;
		if (nested)
			nested->cancel();
		Scheduler::getInstance()->cancel(m_process);
	}

	bool render(Scene *scene, RenderQueue *queue, const RenderJob *job,
			int sceneResID, int sensorResID, int samplerResID) {
		ref<Scheduler> scheduler = Scheduler::getInstance();
		ref<Sensor> sensor = scene->getSensor();
		ref<Sampler> sampler = sensor->getSampler();
		const Film *film = sensor->getFilm();
		size_t nCores = scheduler->getCoreCount();
		size_t sampleCount = sampler->getSampleCount();
		m_config.importanceMap = NULL;

		if (m_config.twoStage && !m_config.firstStage) {
			Log(EInfo, "Executing first MLT stage");
			ref<Timer> timer = new Timer();
			Assert(m_config.firstStageSizeReduction > 0);
			m_config.importanceMap = BidirectionalUtils::mltLuminancePass(
					scene, sceneResID, queue, m_config.firstStageSizeReduction,
					m_nestedJob);
			if (!m_config.importanceMap) {
				Log(EWarn, "First-stage MLT process failed!");
				return false;
			}
			Log(EInfo, "First MLT stage took %i ms", timer->getMilliseconds());
		}

		bool nested = m_config.twoStage && m_config.firstStage;

		Vector2i cropSize = film->getCropSize();;
		Log(EInfo, "Starting %srender job (%ix%i, " SIZE_T_FMT
			" %s, " SSE_STR ", approx. " SIZE_T_FMT " mutations/pixel) ..", 
			nested ? "nested " : "", cropSize.x, cropSize.y,
			nCores, nCores == 1 ? "core" : "cores", sampleCount);

		if (m_config.workUnits <= 0)
			m_config.workUnits = (size_t) std::ceil((cropSize.x 
				* cropSize.y * sampleCount) / 200000.0f);

		m_config.nMutations = (cropSize.x * cropSize.y *
			sampleCount) / m_config.workUnits;

		ref<Bitmap> directImage;
		if (m_config.separateDirect && m_config.directSamples > 0 && !nested) {
			directImage = BidirectionalUtils::renderDirectComponent(scene, 
				sceneResID, sensorResID, queue, job, m_config.directSamples);
			if (directImage == NULL)
				return false;
		}

		ref<ReplayableSampler> rplSampler = new ReplayableSampler();
		ref<PathSampler> pathSampler = new PathSampler(PathSampler::EBidirectional, scene, 
			rplSampler, rplSampler, rplSampler, m_config.maxDepth, 10,
			m_config.separateDirect, true);
		
		std::vector<PathSeed> pathSeeds;
		ref<MLTProcess> process = new MLTProcess(job, queue, 
				m_config, directImage, pathSeeds);

		m_config.luminance = pathSampler->generateSeeds(m_config.luminanceSamples, 
			m_config.workUnits, false, pathSeeds);

		pathSeeds.clear();
	
		m_config.luminance = pathSampler->generateSeeds(m_config.luminanceSamples, 
			m_config.workUnits, true, pathSeeds);

		if (!nested)
			m_config.dump();

		int rplSamplerResID = scheduler->registerResource(rplSampler);

		process->bindResource("scene", sceneResID);
		process->bindResource("sensor", sensorResID);
		process->bindResource("sampler", samplerResID);
		process->bindResource("rplSampler", rplSamplerResID);

		m_process = process;
		scheduler->schedule(process);
		scheduler->wait(process);
		m_process = NULL;
		process->develop();
		scheduler->unregisterResource(rplSamplerResID);

		return process->getReturnStatus() == ParallelProcess::ESuccess;
	}

	MTS_DECLARE_CLASS()
private:
	ref<ParallelProcess> m_process;
	ref<RenderJob> m_nestedJob;
	MLTConfiguration m_config;
};

MTS_IMPLEMENT_CLASS_S(MLT, false, Integrator)
MTS_EXPORT_PLUGIN(MLT, "Path Space Metropolis Light Transport");
MTS_NAMESPACE_END
