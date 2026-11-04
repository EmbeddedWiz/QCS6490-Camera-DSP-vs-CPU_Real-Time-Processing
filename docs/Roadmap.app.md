# Roadmap :: Application Directions
# Application Directions

## 1. Engineering Concept

The QCS6490 project is a foundation for real-time processing of continuous data streams on a heterogeneous computing platform.

The current implementation uses a camera as its input source and compares CPU and Hexagon DSP processing. The broader engineering concept is not camera-specific: acquire data, transform it, extract useful information, evaluate its significance and deliver the result to the next processing stage.

Depending on the application, the input may represent an image, a physical measurement, a machine operating condition or a stream of timestamped events.

The platform combines ARM CPU processing with Hexagon DSP acceleration. Where a supported inference runtime and compatible accelerator are available, an NPU can become another stage in the processing chain.

The purpose of future development is to adapt this foundation to specific engineering problems rather than add features without a defined application.

## 2. Reusable Processing Architecture

A potential application can be organized as a sequence of stages:

**Data acquisition → preprocessing → feature extraction → analysis or inference → decision → output**

The stages need not all run on the same processor.

| Processing resource | Potential responsibility |
|---|---|
| CPU | Device control, scheduling, configuration, state management, event handling and application logic |
| Hexagon DSP/HVX | Filtering, resampling, numerical transformations, image processing and other suitable streaming operations |
| NPU, if supported | Neural-network inference for detection, classification or regression |
| External interfaces | Sensor input, timestamps, communications, storage and delivery of results |

The actual allocation depends on data formats, memory transfers, latency requirements and available software support.

The existing project provides a starting point for investigating this architecture. It does not yet demonstrate every processing stage or every application described below.

## 3. Potential Engineering Applications

### 3.1 Automotive and Vehicle Electronics

**Engineering problem:** Extract reliable information from vehicle sensors under strict timing, environmental and reliability constraints.

Potential applications include:

- Camera preprocessing for driver monitoring, occupant monitoring and vehicle perception.
- Image-quality assessment under changing illumination, motion blur and low-light conditions.
- Sensor-data synchronization and preparation for downstream perception.
- Monitoring of camera pipeline health, dropped frames and processing deadlines.
- Preprocessing before neural-network inference.

The current camera pipeline is relevant to the acquisition, buffering and preprocessing stages. DSP acceleration could handle suitable deterministic image operations, while an NPU could perform supported perception models.

**Engineering challenge:** A useful image is not necessarily a reliable perception input. Exposure changes, motion, glare, timing errors and data loss can affect downstream decisions. The engineering task is to quantify these effects and detect when the input is no longer suitable.

**Required extension:** Vehicle-relevant datasets, additional sensors where needed, synchronization analysis, end-to-end latency measurements, fault handling and validation under defined operating conditions.

The current project is not an automotive-qualified system and does not establish compliance with ISO 26262, SOTIF or automotive cybersecurity requirements.

### 3.2 Medical Imaging and Diagnostic Instrumentation

**Engineering problem:** Transform raw measurements from medical imaging sensors into reliable, reproducible data suitable for visualization, quantitative assessment and further clinical interpretation. Processing must preserve diagnostically relevant information while controlling noise, acquisition artefacts and measurement variability.

Potential engineering applications include:

- **Ultrasound image enhancement:** Reduce speckle noise while preserving tissue boundaries and small anatomical structures required for examination and measurement.

- **Low-dose X-ray image processing:** Suppress acquisition noise and compensate for reduced signal levels while preserving fine details relevant to detecting small structures and abnormalities.

- **CT image reconstruction support:** Perform preprocessing and correction of projection data, including noise reduction and artefact mitigation, before or during reconstruction workflows.

- **MRI acquisition and reconstruction:** Process multi-channel measurement data, support signal conditioning and investigate acceleration of computationally intensive reconstruction stages.

- **Image artefact detection and quality assessment:** Identify motion-related degradation, incomplete acquisitions, saturation, abnormal signal levels and other conditions that may make an image unsuitable for reliable interpretation.

- **Quantitative image analysis:** Prepare images for measuring anatomical dimensions, distances, areas, volumes and changes between successive examinations, with attention to spatial calibration and measurement consistency.

- **Real-time guidance and visualization:** Process and display imaging data with predictable latency in ultrasound-guided procedures, endoscopic systems and other interactive diagnostic instruments.

- **AI-assisted diagnostic preprocessing:** Prepare normalized, quality-controlled image data for inference models performing segmentation, anatomical structure detection or classification, while preserving the distinction between preprocessing and clinically validated diagnostic decisions.

The transferable capability is the processing pipeline: managing continuous input, applying numerical transformations, maintaining buffer integrity and measuring processing latency.

**Engineering challenge:** Processing speed alone is insufficient. Measurement repeatability, calibration, uncertainty, noise behavior and traceability can matter more than visual appearance.

**Required extension:** The relevant medical sensor, calibration procedures, reference datasets, uncertainty analysis and application-specific verification.

The QCS6490 camera demonstration is not a medical device and does not establish diagnostic accuracy or regulatory compliance.

### 3.3 Power Electronics, Electrical Energy Systems and Electromechanical Automation

**Engineering problem:** Develop heterogeneous edge-processing systems for advanced condition monitoring, fault diagnostics and degradation assessment of complex electrical and electromechanical installations using synchronized multi-sensor data.

Potential engineering applications include:

- **Partial discharge analysis and insulation-defect localization:** Process synchronized UHF, HFCT, TEV and acoustic measurements from high-voltage transformers, switchgear and cable systems. Perform pulse detection, phase-resolved pattern analysis, spectral characterization and multi-sensor time-of-arrival correlation to classify discharge activity and estimate defect location.

- **SiC/GaN power module health monitoring:** Analyze high-resolution switching waveforms, gate-voltage characteristics, on-state voltage, switching transients and thermal measurements. Extract degradation indicators associated with gate-oxide aging, bond-wire and interconnect fatigue, increased conduction losses and thermal-path deterioration.

- **Power converter fault reconstruction:** Correlate synchronized three-phase voltage and current waveforms, DC-link measurements, PWM and gate-driver event records, load conditions and thermal data. Analyze switching transients, commutation abnormalities, phase asymmetry and DC-bus instability to distinguish supply disturbances, control-loop instability and power-stage faults.

- **Large rotating machinery diagnostics:** Apply order tracking, envelope analysis, cyclostationary processing and time-frequency decomposition to synchronized vibration, acoustic, motor-current, shaft-speed and torque measurements. Diagnose bearing defects, rotor eccentricity, broken rotor bars, gear-mesh damage, shaft misalignment and mechanical resonance under variable-load and variable-speed conditions.

- **Grid disturbance and converter interaction analysis:** Correlate synchronized measurements from multiple network locations to investigate harmonic and interharmonic propagation, resonances, converter-induced oscillations, transient disturbances and interactions between grid-connected power converters. Identify disturbance sources and their propagation across industrial power networks.

- **Multimodal inspection of high-voltage equipment:** Fuse visible-light and thermal imaging with UHF, ultrasonic, acoustic and electrical measurements for condition assessment of transformers, busbars, cable terminations, insulators and switchgear. Correlate thermal anomalies, discharge signatures and visual defects to support fault localization and equipment-condition classification.

- **Multi-axis servo and industrial machinery diagnostics:** Correlate synchronized position, velocity, torque, motor-current, vibration and machine-vision data across CNC machines, robotic cells and automated production lines. Identify backlash, axis synchronization errors, mechanical resonance, abnormal friction, tool wear, chatter and process-related deviations.

- **Electrothermal modeling and lifetime assessment:** Combine load profiles, switching-event statistics, semiconductor losses, measured temperatures and cooling-system data with reduced-order electrothermal models. Estimate internal junction temperatures, thermal impedance changes, cooling degradation and accumulated thermal-cycling stress to assess component degradation and support predictive maintenance.

These workloads involve multi-channel acquisition, high-resolution signal processing, time-frequency analysis, physical-model-based estimation, sensor fusion and machine-learning inference. They provide opportunities for heterogeneous execution across CPU, DSP/HVX, GPU and supported NPU workloads.

High-bandwidth synchronized acquisition requires dedicated ADC, FPGA or other measurement front ends. Deterministic converter control, gate driving and fast electrical protection remain functions of dedicated real-time controllers and protection hardware.

DSP processing is potentially useful for filtering, windowing, spectral transforms and feature extraction. The CPU can manage operating modes, alarms and higher-level analysis. An NPU may be useful for supported learned classifiers, but is not necessary for conventional signal processing.

**Engineering challenge:** Electrical and mechanical systems may require precise timestamps, bounded latency, appropriate sampling rates, isolation and reliable behavior during transients. An average processing time does not establish that every sample can be handled before its deadline.

**Required extension:** Suitable acquisition hardware, signal conditioning, sampling and synchronization design, reference measurements and tests using representative operating conditions.

The current camera hardware does not directly provide electrical power measurements. Those require additional sensors and acquisition interfaces.

### 3.4 Industrial Automation, Machine Vision and Edge Processing

**Engineering problem:** Develop compact, heterogeneous edge-computing nodes for production inspection, multi-camera identification, 3D measurement and automated material handling. The focus is on replacing complex or expensive vision workstations with integrated local processing systems.

Potential engineering applications include:

- **Multi-camera barcode identification and 3D dimensioning (DWS):** Develop a conveyor-mounted inspection station using 3–4 industrial cameras, including a calibrated stereo pair, for simultaneous 1D/2D barcode reading, label localization, parcel identification and 3D dimensional measurement. Combine multi-view images, depth reconstruction, encoder position and optional weighing data to associate each parcel with its tracking ID, dimensions, orientation and visual record. Handle labels on different package faces, irregular shapes and partially obscured codes. The system can provide DWS functionality (Dimension–Weigh–Scan) for warehouse receiving, shipping and distribution operations.

- **Vision-based conveyor sorting:** Implement a compact sorting station for parcels, containers, components or recyclable materials. Use stereo vision and multi-view classification to determine object dimensions, shape, orientation and category while tracking its position along the conveyor using encoder feedback. Calculate destination lanes and actuator timing, then transmit sorting commands to a PLC controlling pneumatic pushers, diverters or motorized gates. Unidentified, damaged or incorrectly classified items can be routed to a reject lane or manual inspection. This architecture is particularly suitable for small and medium-sized distribution facilities and production lines requiring flexible, low-cost automation.

- **CNC process and tool-condition monitoring:** Combine spindle vibration, acoustic-emission signals, drive telemetry and optical tool inspection on 5-axis machining centres. Apply order tracking, spectral analysis and sensor fusion to identify regenerative chatter, insert chipping, tool wear and abnormal cutting conditions. Correlate detected events with spindle speed, feed rate and machining operations.

- **Laser welding and additive manufacturing monitoring:** Integrate coaxial process cameras, thermal imaging and machine telemetry into laser-welding cells for battery busbars, automotive structures and precision components. Analyze melt-pool geometry, seam position, spatter and thermal signatures; correlate deviations with laser power, scanning speed and robot trajectory.

- **3D robotic perception and machine tending:** Process stereo-camera or structured-light data for point-cloud reconstruction, CAD registration, 6D pose estimation and grasp-point generation. Applications include bin picking of cast housings, forged blanks and randomly oriented machined components. Return object coordinates and grasp candidates to ABB, FANUC or KUKA controllers for trajectory planning and execution.

- **Electronics and power-module assembly inspection:** Perform multi-view inspection of PCB assemblies, SiC power modules and electric-drive components. Measure component displacement, terminal coplanarity, solder-paste geometry, busbar alignment and laser-weld geometry. Combine optical and 3D measurements with production records for defect classification and serial-number-level traceability.

These applications require concurrent camera acquisition, stereo reconstruction, image registration, geometric measurement, object tracking, parallel image processing and AI inference. CPU, DSP/HVX, GPU and supported NPU workloads can be distributed within a single edge-computing node.

The QCS6490 can act as a local vision and analytics controller connected to industrial cameras, measurement interfaces and machine telemetry. PLCs retain conveyor sequencing, actuator control, interlocks and safety functions. Camera connectivity, encoder synchronization and aggregate data bandwidth depend on the selected carrier board and external interfaces.

**Engineering challenge:** A laboratory demonstration may work on a representative image and still fail in production because of changing illumination, product variation, vibration, contamination or throughput requirements.

The relevant performance measures are not only FPS but also detection accuracy, false-rejection rate, missed defects, latency distribution and repeatability.

**Required extension:** Representative production samples, defined acceptance criteria, lighting and optics appropriate to the task, and integration with the actual control system.

The current platform is a potential processing node, not a replacement for a deterministic industrial controller or a safety-rated control system.

### 3.5 Financial Markets, Trading Infrastructure and Quantitative Computing

**Engineering problem:** Deploy compact, low-power Linux-based computing nodes for local market analytics, portfolio risk, execution controls and energy-market optimization. Target applications operate on filtered market-data streams and require continuous numerical processing, multi-instrument state management and local decision support, rather than exchange-level microsecond execution.

Potential engineering applications include:

- **Multi-asset market-data and spread analytics appliance:** Deploy a dedicated node at a proprietary trading desk to consume normalized CME, Eurex or other exchange feeds for selected futures, options, equities and FX instruments. Maintain selected order-book depth, calculate calendar and intercommodity spreads, yield-curve butterflies, cash-futures basis and cross-market deviations. Generate fair-value estimates, liquidity indicators and alerts for medium-frequency systematic strategies. Raw exchange-feed arbitration and recovery remain upstream.

- **Derivatives portfolio and intraday risk appliance:** Maintain a local representation of futures, options, options-on-futures and multi-leg positions. Recalculate Greeks, volatility-surface parameters, spread exposure and scenario P&L across underlying-price, volatility and time-decay shocks. Support desk-level exposure aggregation, concentration monitoring and margin-sensitivity analysis against exchange methodologies such as CME SPAN 2. The node provides an independent risk view rather than replacing a clearinghouse margin engine.

- **Pre-trade risk gateway for systematic strategies:** Insert an independent validation service between a local strategy engine or OMS and the broker FIX gateway. Evaluate order size, price collars, position limits, gross and net exposure, derivatives Greeks, duplicate submissions and stale-market conditions. Apply strategy- and account-level limits, event logging and controlled order blocking. This is intended for moderate-throughput execution systems with millisecond-scale processing budgets, not HFT order routing.

- **Market-data capture and execution replay appliance:** Install a dedicated node at a trading site to record selected market-data updates, order submissions, acknowledgements, executions and risk decisions with synchronized timestamps. Maintain encrypted local archives and support deterministic event replay for execution analysis, slippage attribution, strategy debugging and post-trade surveillance. Separate capture and replay workloads from the live execution path.

- **Battery energy storage and renewable-asset trading gateway:** Deploy an edge-computing node at a grid-scale BESS or renewable generation site. Combine SCADA telemetry, state of charge, battery power limits, thermal conditions, degradation estimates, generation forecasts and day-ahead/intraday market prices. Run rolling dispatch and bidding optimization across energy and balancing-market time windows, delivering target schedules and operating constraints to the site EMS or central aggregator. The local node connects physical asset constraints with commercial market decisions without performing exchange-level execution.

The QCS6490 can provide the compute resources for these applications in a compact, low-power appliance. CPU cores handle feed normalization, event processing, numerical pricing and risk logic; GPU resources can be evaluated for parallel scenario calculations; NPU inference may support selected forecasting and classification models. Hardware cryptographic services can protect remote connectivity and local records, subject to Linux driver support.

The intended deployment is a distributed analytics and control-support layer: one node per trading desk, instrument group, remote site or independent risk domain. Multiple nodes can partition workloads horizontally, but should not be treated as a tightly coupled HPC cluster or a substitute for FPGA-based feed handlers and execution infrastructure.

**Engineering challenge:** In latency-sensitive trading, tail latency, event ordering, timestamp accuracy, feed gaps, determinism and execution costs can dominate raw arithmetic throughput.

The QCS6490 camera project does not demonstrate exchange connectivity, market-data handling, trading execution or low-latency trading performance. Those would be separate engineering projects with their own hardware, network architecture and validation.

### 3.6 Defence, Military Sensor Systems and Ruggedized Edge Computing

**Engineering problem:** Deploy compact, low-power heterogeneous computing nodes for multi-sensor processing, electro-optical system support, local video exploitation, equipment diagnostics and secure data handling in bandwidth-constrained and infrastructure-degraded environments.

Potential engineering applications include:

- **Distributed electro-optical sensor diagnostics — F-35 DAS:** Ground-support and maintenance equipment for multi-channel infrared sensor systems. Process synchronized camera outputs to evaluate image integrity, dead-pixel patterns, channel consistency, frame timing and sensor degradation. The F-35 AN/AAQ-37 DAS provides a relevant example of a six-sensor infrared architecture. QCS6490 could support a maintenance analysis station for selected sensor channels, not replace the aircraft's six-channel mission processor.

- **Armoured vehicle crew-vision processing — Boxer / Rheinmetall SAS:** Auxiliary multi-camera processing for 360° crew visibility and driver situational awareness. Combine visible and infrared camera streams, perform image stitching, perspective correction, low-light enhancement and local video recording. The Boxer already incorporates external 360° camera coverage, while Rheinmetall's SAS architecture uses networked electro-optical/IR modules and dedicated video processing. A QCS6490-based node could provide an additional modular vision-processing channel or a development platform for secondary crew displays.

- **Tactical UAS video exploitation — RQ-20B Puma:** Portable edge-processing equipment at a field ground-control or video-analysis station. Decode EO/IR downlink streams, synchronize video with platform metadata, generate compressed recordings, maintain event-indexed archives and encrypt stored data before transfer over constrained communication links. Hardware video processing is particularly relevant when handling several concurrent video sources without requiring a conventional workstation at every field location.

- **Field maintenance and diagnostics — M109A7 Paladin:** An independent maintainer's computing unit connected through approved diagnostic interfaces to the vehicle's onboard health-management system. Correlate subsystem fault records with thermal and visible-light inspection of electrical compartments, connectors, cable assemblies and mechanical equipment. Apply local image analysis to inspection records, associate findings with maintenance history and provide encrypted diagnostic archives. The M109A7 already incorporates a 70 kW high-voltage electrical architecture, onboard diagnostics, automated self-tests and Interactive Electronic Technical Manuals.

- **Shipboard damage-control monitoring — USS Bonhomme Richard / LHD-class vessels:** Supplementary local monitoring of maintenance areas and ship compartments using distributed visible-light and thermal cameras. Combine camera streams with temperature, smoke and environmental measurements, retain synchronized incident records and provide local visualization independently of the central network. The July 2020 USS Bonhomme Richard fire investigation identified degraded heat-detection capability, communications and firefighting-system readiness among the contributing material-condition problems. Such a node would supplement, never replace, certified fire detection and damage-control systems.

- **Protected tactical communications and multi-sensor data gateway:** Integrate a QCS6490-based computing module into a vehicle-mounted or mobile command-post communications subsystem, between local EO/IR cameras, radar data interfaces and tactical radio or SATCOM terminals. Provide encrypted IP data transport, authenticated remote connectivity, prioritized video and sensor-data forwarding, local recording and store-and-forward operation during intermittent connectivity. Hardware AES-GCM acceleration, secure boot and TrustZone can support the data-processing and security architecture. Example integration targets include network extensions to L3Harris AN/PRC-163 or AN/PRC-167 tactical radio systems. The module acts as a sensor-data gateway and video-processing node, not as a replacement for certified COMSEC equipment or the radio's cryptographic subsystem.

- **Radar, RF and EO/IR perimeter-surveillance fusion:** Deploy an edge-computing module at a remote military installation, forward operating base, protected depot or mobile surveillance post. Ingest processed radar plots and tracks from systems such as HENSOLDT SPEXER 2000 3D MKIII or Blighter A400, passive RF detection reports and identification-status data from dedicated IFF equipment. Correlate these inputs with multiple visible-light and thermal camera streams to provide track-to-image association, event classification, operator alerts and synchronized evidence recording. Applications include perimeter intrusion monitoring, low-altitude airspace surveillance and detection of unauthorized unmanned aircraft. The node performs multi-sensor data fusion and local video analytics; radar waveform processing, RF direction finding, IFF interrogation and any response or engagement functions remain with dedicated equipment.

The QCS6490 provides a combination of multi-camera ISP, hardware video encoding/decoding, CPU, GPU, Hexagon DSP/HVX and AI acceleration in a typical 6–9 W power envelope. Its five four-lane MIPI-CSI interfaces and support for up to five concurrent cameras are relevant to compact multi-sensor equipment. The AI Engine provides up to 12 dense TOPS for supported inference workloads.

Hardware-assisted AES, hashing and message authentication can support encrypted local storage and secure data transport where the corresponding Linux cryptographic drivers and security architecture are available.

The intended role is a modular edge-processing or maintenance-support computer, not a flight-critical avionics computer, weapon-control processor or replacement for dedicated military mission computing. Environmental qualification, electromagnetic compatibility, secure boot, key management, authenticated updates and system-level security certification remain separate engineering requirements.

## 4. What Can Be Reused and What Must Change

| Existing foundation | Potential reuse | Additional work |
|---|---|---|
| V4L2 camera acquisition | Compatible camera-based applications | Different sensors, drivers or acquisition interfaces for other data sources |
| DMA-BUF and buffer ownership | Shared-memory data paths where supported | Interface-specific memory handling and lifetime rules |
| CPU processing engine | Control logic and numerical reference implementations | Domain-specific algorithms |
| Hexagon DSP/HVX integration | Suitable streaming and vector workloads | Algorithm-specific optimization and profiling |
| CPU/DSP parity tests | Cross-implementation numerical validation | Reference data and acceptance criteria for each application |
| GStreamer and Wayland output | Visualization and supported multimedia paths | Different output protocols or non-visual result handling |
| Event loop and diagnostics | Application coordination and observability | Domain-specific timing, recovery and system integration |
| Existing build structure | Reproducible development workflow | Toolchains, dependencies and deployment requirements of the target system |

Reuse is strongest at the architectural and processing-framework levels. It does not imply that a camera-specific implementation can directly process arbitrary sensors without modification.

## 5. Heterogeneous Processing as a Pipeline

CPU, DSP and NPU should be treated as complementary resources rather than competing implementations of the same function.

A possible future architecture is:

1. Acquire data from one or more sources.
2. Use the CPU to manage devices, timestamps and processing state.
3. Apply DSP-based filtering, transformations or feature extraction where profiling justifies it.
4. Send suitable data to a supported NPU for inference, if the application requires it.
5. Evaluate the result on the CPU and deliver it to the relevant application or control interface.

This is a candidate architecture, not a description of a currently implemented CPU–DSP–NPU pipeline.

Its feasibility depends on accelerator availability, runtime support, compatible memory interfaces, transfer overhead and end-to-end latency. The optimal partition must be established through measurement.

## 6. Choosing a Future Direction

Each future application should begin with a concrete engineering problem rather than a list of desired features.

A proposed direction should define:

- The physical or logical quantity being measured.
- The input source and required acquisition hardware.
- The processing stages and their allocation to available compute resources.
- The latency, accuracy, throughput or reliability requirements.
- The reference data and method used to verify the result.
- The additional hardware, software and validation work required.

The next development stage should then implement one bounded use case and demonstrate its acceptance criteria.

## 7. Current Position

The present project establishes a camera-based reference for continuous data acquisition, image processing, CPU/DSP comparison and multimedia integration on Qualcomm QCS6490.

Its broader value is the engineering foundation for building and evaluating streaming-data pipelines on a heterogeneous embedded platform.

The future application determines what the input means, which algorithms are required, how the compute resources should be allocated and what constitutes a correct result. The current project supplies part of that foundation; each new domain still requires its own sensors, domain knowledge and validation.
