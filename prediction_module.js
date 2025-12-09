/*
 * ========================================
 * Smart Study Space - Flow State Prediction Module
 * Pure JavaScript Implementation
 * ========================================
 * 
 * Features:
 * 1. Historical pattern analysis
 * 2. Time-series based prediction
 * 3. Confidence scoring
 * 4. Next 24-hour flow state forecasting
 */

// ========== Configuration ==========
const PREDICTION_CONFIG = {
    PREDICTION_WINDOW: 24,        // Predict next 24 hours
    HISTORICAL_DAYS: 7,           // Use last 7 days of data
    MIN_DATA_POINTS: 100,         // Minimum data points needed
    FLOW_STATE_THRESHOLD: 0.3,    // 30% probability threshold
    TOP_PREDICTIONS: 5,           // Show top N predictions
    UPDATE_INTERVAL: 3600000      // Update every hour (ms)
};

// ========== Thresholds for State Classification ==========
const STATE_THRESHOLDS = {
    NOISE_THRESHOLD: 55.0,        // dB
    BPM_LOW: 60,
    BPM_HIGH: 80,
    HRV_THRESHOLD: 700,           // ms (R-R interval)
    BPM_MIN_VALID: 40,
    BPM_MAX_VALID: 200
};

// ========== Global Prediction Data ==========
let predictionData = {
    predictions: [],
    lastUpdate: null,
    confidence: 0,
    dataQuality: 'insufficient'
};

// ========== Main Prediction Function ==========
async function runPredictionAnalysis() {
    console.log('[Prediction] Starting flow state prediction analysis...');
    
    try {
        // Step 1: Fetch historical data
        const historicalData = await fetchPredictionHistoricalData();
        
        if (!historicalData || historicalData.length < PREDICTION_CONFIG.MIN_DATA_POINTS) {
            console.log('[Prediction] Insufficient data. Need at least', 
                       PREDICTION_CONFIG.MIN_DATA_POINTS, 'points. Current:', 
                       historicalData ? historicalData.length : 0);
            updatePredictionUINoData(historicalData ? historicalData.length : 0);
            return null;
        }
        
        console.log('[Prediction] Processing', historicalData.length, 'data points');
        
        // Step 2: Analyze historical patterns
        const patterns = analyzeTemporalPatterns(historicalData);
        
        // Step 3: Generate predictions
        const predictions = generateFlowStatePredictions(patterns);
        
        // Step 4: Calculate overall confidence
        const overallConfidence = calculateOverallConfidence(predictions, historicalData.length);
        
        // Step 5: Store results
        predictionData.predictions = predictions;
        predictionData.lastUpdate = new Date();
        predictionData.confidence = overallConfidence;
        predictionData.dataQuality = assessDataQuality(historicalData.length);
        
        // Step 6: Update UI
        updatePredictionUI(predictions, overallConfidence);
        
        console.log('[Prediction] Analysis complete. Found', predictions.length, 'potential flow states');
        
        return predictions;
        
    } catch (error) {
        console.error('[Prediction] Error during analysis:', error);
        updatePredictionUIError();
        return null;
    }
}

// ========== Data Fetching ==========
async function fetchPredictionHistoricalData() {
    const daysAgo = PREDICTION_CONFIG.HISTORICAL_DAYS;
    const estimatedResults = daysAgo * 24 * 60 / 5;  // Assuming 5-min upload intervals
    
    try {
        const url = `https://api.thingspeak.com/channels/2812071/feeds.json?api_key=TBTU2BQOMZ8RE8GE&results=${estimatedResults}`;
        const response = await fetch(url);
        
        if (!response.ok) {
            throw new Error('Failed to fetch data from ThingSpeak');
        }
        
        const data = await response.json();
        
        if (!data.feeds || data.feeds.length === 0) {
            return null;
        }
        
        // Parse and clean data
        const cleanData = data.feeds.map(feed => {
            const timestamp = new Date(feed.created_at);
            const noise = parseFloat(feed.field2) || 0;
            const rrInterval = parseFloat(feed.field3) || 0;  // Latest R-R interval
            const bpmAvg = parseInt(feed.field5) || 0;
            const bpmRealtime = parseInt(feed.field6) || 0;
            
            // Determine which BPM to use
            const bpm = bpmAvg > 0 ? bpmAvg : bpmRealtime;
            
            // Classify state
            const state = classifyStateForPrediction(noise, bpm, rrInterval);
            
            return {
                timestamp: timestamp,
                hour: timestamp.getHours(),
                dayOfWeek: timestamp.getDay(),
                date: timestamp.toISOString().split('T')[0],
                noise: noise,
                bpm: bpm,
                rrInterval: rrInterval,
                state: state,
                isFlowState: state === 'Flow State'
            };
        }).filter(d => d.bpm >= STATE_THRESHOLDS.BPM_MIN_VALID && 
                      d.bpm <= STATE_THRESHOLDS.BPM_MAX_VALID);
        
        console.log('[Prediction] Cleaned data points:', cleanData.length);
        
        return cleanData;
        
    } catch (error) {
        console.error('[Prediction] Data fetch error:', error);
        return null;
    }
}

// ========== State Classification ==========
function classifyStateForPrediction(noise, bpm, rrInterval) {
    if (bpm < STATE_THRESHOLDS.BPM_MIN_VALID || bpm > STATE_THRESHOLDS.BPM_MAX_VALID) {
        return 'Standby';
    }
    
    const isQuiet = noise < STATE_THRESHOLDS.NOISE_THRESHOLD;
    
    // Determine good physiological state
    // Good state: R-R interval high (relaxed) OR heart rate in optimal range
    const hasGoodHRV = rrInterval > STATE_THRESHOLDS.HRV_THRESHOLD;
    const hasOptimalBPM = (bpm >= STATE_THRESHOLDS.BPM_LOW && bpm <= STATE_THRESHOLDS.BPM_HIGH);
    const isGoodPhysio = hasGoodHRV || hasOptimalBPM;
    
    // Four-quadrant classification
    if (isQuiet && isGoodPhysio) {
        return 'Flow State';        // Target state
    } else if (!isQuiet && isGoodPhysio) {
        return 'Normal Learning';
    } else if (isQuiet && !isGoodPhysio) {
        return 'Standby';
    } else {
        return 'Distracted';
    }
}

// ========== Pattern Analysis ==========
function analyzeTemporalPatterns(data) {
    // Initialize pattern storage
    const hourlyPatterns = {};
    
    // Create pattern structure for each hour of each day
    for (let day = 0; day < 7; day++) {
        for (let hour = 0; hour < 24; hour++) {
            const key = `${day}-${hour}`;
            hourlyPatterns[key] = {
                day: day,
                hour: hour,
                flowStateCount: 0,
                totalCount: 0,
                noiseSum: 0,
                bpmSum: 0,
                rrIntervalSum: 0,
                samples: []
            };
        }
    }
    
    // Aggregate historical data
    data.forEach(entry => {
        const key = `${entry.dayOfWeek}-${entry.hour}`;
        
        if (hourlyPatterns[key]) {
            hourlyPatterns[key].totalCount++;
            hourlyPatterns[key].noiseSum += entry.noise;
            hourlyPatterns[key].bpmSum += entry.bpm;
            hourlyPatterns[key].rrIntervalSum += entry.rrInterval;
            
            if (entry.isFlowState) {
                hourlyPatterns[key].flowStateCount++;
            }
            
            // Store sample for detailed analysis
            hourlyPatterns[key].samples.push({
                timestamp: entry.timestamp,
                state: entry.state
            });
        }
    });
    
    // Calculate statistics for each pattern
    Object.keys(hourlyPatterns).forEach(key => {
        const pattern = hourlyPatterns[key];
        
        if (pattern.totalCount > 0) {
            pattern.avgNoise = pattern.noiseSum / pattern.totalCount;
            pattern.avgBPM = pattern.bpmSum / pattern.totalCount;
            pattern.avgRRInterval = pattern.rrIntervalSum / pattern.totalCount;
            pattern.probability = pattern.flowStateCount / pattern.totalCount;
            
            // Calculate consistency (standard deviation of outcomes)
            const variance = pattern.samples.reduce((sum, sample) => {
                const isFlow = sample.state === 'Flow State' ? 1 : 0;
                return sum + Math.pow(isFlow - pattern.probability, 2);
            }, 0) / pattern.totalCount;
            
            pattern.consistency = 1 - Math.sqrt(variance);
        } else {
            pattern.avgNoise = 0;
            pattern.avgBPM = 0;
            pattern.avgRRInterval = 0;
            pattern.probability = 0;
            pattern.consistency = 0;
        }
    });
    
    console.log('[Prediction] Pattern analysis complete for', Object.keys(hourlyPatterns).length, 'time slots');
    
    return hourlyPatterns;
}

// ========== Prediction Generation ==========
function generateFlowStatePredictions(patterns) {
    const now = new Date();
    const predictions = [];
    
    // Generate predictions for next PREDICTION_WINDOW hours
    for (let i = 1; i <= PREDICTION_CONFIG.PREDICTION_WINDOW; i++) {
        const futureTime = new Date(now.getTime() + i * 60 * 60 * 1000);
        const futureDay = futureTime.getDay();
        const futureHour = futureTime.getHours();
        
        const key = `${futureDay}-${futureHour}`;
        const pattern = patterns[key];
        
        // Only include if probability exceeds threshold
        if (pattern && pattern.probability >= PREDICTION_CONFIG.FLOW_STATE_THRESHOLD) {
            const confidence = calculatePredictionConfidence(pattern);
            
            predictions.push({
                time: futureTime,
                hour: futureHour,
                dayOfWeek: futureDay,
                probability: pattern.probability,
                confidence: confidence,
                expectedNoise: pattern.avgNoise,
                expectedBPM: pattern.avgBPM,
                expectedRRInterval: pattern.avgRRInterval,
                sampleSize: pattern.totalCount,
                consistency: pattern.consistency,
                hoursFromNow: i
            });
        }
    }
    
    // Sort by combined score (probability * confidence)
    predictions.sort((a, b) => {
        const scoreA = a.probability * (a.confidence / 100);
        const scoreB = b.probability * (b.confidence / 100);
        return scoreB - scoreA;
    });
    
    // Return top predictions
    return predictions.slice(0, PREDICTION_CONFIG.TOP_PREDICTIONS);
}

// ========== Confidence Calculation ==========
function calculatePredictionConfidence(pattern) {
    // Factors affecting confidence:
    // 1. Sample size (more samples = higher confidence)
    // 2. Consistency (less variance = higher confidence)
    // 3. Probability strength
    
    const MIN_SAMPLES_FOR_CONFIDENCE = 10;
    const IDEAL_SAMPLES = 50;
    
    // Sample size factor (0 to 1)
    const sampleFactor = Math.min(pattern.totalCount / IDEAL_SAMPLES, 1.0);
    
    // Consistency factor (already 0 to 1)
    const consistencyFactor = pattern.consistency;
    
    // Probability strength factor (0 to 1)
    const probabilityFactor = pattern.probability;
    
    // Minimum sample requirement
    if (pattern.totalCount < MIN_SAMPLES_FOR_CONFIDENCE) {
        return 0;
    }
    
    // Weighted combination
    const confidence = (
        sampleFactor * 0.4 + 
        consistencyFactor * 0.3 + 
        probabilityFactor * 0.3
    ) * 100;
    
    return Math.round(confidence);
}

function calculateOverallConfidence(predictions, dataPoints) {
    if (predictions.length === 0) {
        return 0;
    }
    
    // Average confidence of all predictions
    const avgConfidence = predictions.reduce((sum, p) => sum + p.confidence, 0) / predictions.length;
    
    // Data quantity factor
    const dataFactor = Math.min(dataPoints / 500, 1.0);  // 500 as ideal
    
    // Combined confidence
    return Math.round(avgConfidence * 0.7 + dataFactor * 30);
}

// ========== Data Quality Assessment ==========
function assessDataQuality(dataPoints) {
    if (dataPoints < 100) return 'insufficient';
    if (dataPoints < 300) return 'low';
    if (dataPoints < 800) return 'moderate';
    if (dataPoints < 1500) return 'good';
    return 'excellent';
}

// ========== UI Update Functions ==========
function updatePredictionUI(predictions, overallConfidence) {
    const container = document.getElementById('predictionContainer');
    if (!container) {
        console.warn('[Prediction] Container element not found');
        return;
    }
    
    container.innerHTML = '';
    
    if (!predictions || predictions.length === 0) {
        container.innerHTML = `
            <div class="prediction-message info">
                <div class="message-icon">INFO</div>
                <div class="message-content">
                    <strong>No High-Probability Flow States Detected</strong>
                    <p>Based on historical patterns, no time slots in the next 24 hours show strong likelihood of flow state conditions. Continue collecting data for more accurate predictions.</p>
                </div>
            </div>
        `;
        return;
    }
    
    // Create prediction cards
    predictions.forEach((pred, index) => {
        const card = createPredictionCard(pred, index);
        container.appendChild(card);
    });
    
    // Update metadata
    updatePredictionMetadata(overallConfidence);
}

function createPredictionCard(prediction, rank) {
    const card = document.createElement('div');
    card.className = 'prediction-card';
    
    const qualityClass = getQualityClass(prediction.confidence);
    
    card.innerHTML = `
        <div class="prediction-header">
            <div class="prediction-rank">Rank ${rank + 1}</div>
            <div class="prediction-quality ${qualityClass}">${prediction.confidence}% Confidence</div>
        </div>
        
        <div class="prediction-time-display">
            <div class="time-value">${prediction.time.toLocaleTimeString('en-US', {
                hour: '2-digit',
                minute: '2-digit',
                hour12: true
            })}</div>
            <div class="time-date">${prediction.time.toLocaleDateString('en-US', {
                weekday: 'short',
                month: 'short',
                day: 'numeric'
            })}</div>
            <div class="time-relative">${prediction.hoursFromNow} hours from now</div>
        </div>
        
        <div class="prediction-metrics-grid">
            <div class="metric-cell">
                <div class="metric-label">Flow Probability</div>
                <div class="metric-value">${(prediction.probability * 100).toFixed(0)}%</div>
            </div>
            <div class="metric-cell">
                <div class="metric-label">Consistency</div>
                <div class="metric-value">${(prediction.consistency * 100).toFixed(0)}%</div>
            </div>
            <div class="metric-cell">
                <div class="metric-label">Sample Size</div>
                <div class="metric-value">${prediction.sampleSize}</div>
            </div>
        </div>
        
        <div class="prediction-expected">
            <div class="expected-title">Expected Conditions:</div>
            <div class="expected-values">
                Noise: ${prediction.expectedNoise.toFixed(1)} dB | 
                Heart Rate: ${prediction.expectedBPM.toFixed(0)} BPM
            </div>
        </div>
    `;
    
    return card;
}

function getQualityClass(confidence) {
    if (confidence >= 70) return 'quality-high';
    if (confidence >= 50) return 'quality-medium';
    return 'quality-low';
}

function updatePredictionMetadata(overallConfidence) {
    const metadataElement = document.getElementById('predictionMetadata');
    if (!metadataElement) return;
    
    const lastUpdateTime = predictionData.lastUpdate.toLocaleTimeString('en-US', {
        hour: '2-digit',
        minute: '2-digit'
    });
    
    const qualityLabel = predictionData.dataQuality.charAt(0).toUpperCase() + 
                        predictionData.dataQuality.slice(1);
    
    metadataElement.innerHTML = `
        <div class="metadata-item">
            <span class="metadata-label">Last Updated:</span>
            <span class="metadata-value">${lastUpdateTime}</span>
        </div>
        <div class="metadata-item">
            <span class="metadata-label">Overall Confidence:</span>
            <span class="metadata-value">${overallConfidence}%</span>
        </div>
        <div class="metadata-item">
            <span class="metadata-label">Data Quality:</span>
            <span class="metadata-value">${qualityLabel}</span>
        </div>
    `;
}

function updatePredictionUINoData(currentDataPoints) {
    const container = document.getElementById('predictionContainer');
    if (!container) return;
    
    const needed = PREDICTION_CONFIG.MIN_DATA_POINTS - currentDataPoints;
    const percentage = Math.round((currentDataPoints / PREDICTION_CONFIG.MIN_DATA_POINTS) * 100);
    
    container.innerHTML = `
        <div class="prediction-message info">
            <div class="message-icon">DATA</div>
            <div class="message-content">
                <strong>Collecting Historical Data</strong>
                <p>Prediction requires at least ${PREDICTION_CONFIG.MIN_DATA_POINTS} data points. 
                Current progress: ${currentDataPoints} / ${PREDICTION_CONFIG.MIN_DATA_POINTS} (${percentage}%)</p>
                <div class="progress-bar">
                    <div class="progress-fill" style="width: ${percentage}%"></div>
                </div>
                <p class="progress-note">Approximately ${needed} more readings needed. Check back in ${Math.ceil(needed / 12)} hours.</p>
            </div>
        </div>
    `;
    
    // Update metadata to show waiting state
    const metadataElement = document.getElementById('predictionMetadata');
    if (metadataElement) {
        metadataElement.innerHTML = `
            <div class="metadata-item">
                <span class="metadata-label">Status:</span>
                <span class="metadata-value">Awaiting Sufficient Data</span>
            </div>
        `;
    }
}

function updatePredictionUIError() {
    const container = document.getElementById('predictionContainer');
    if (!container) return;
    
    container.innerHTML = `
        <div class="prediction-message error">
            <div class="message-icon">ERROR</div>
            <div class="message-content">
                <strong>Prediction Error</strong>
                <p>Unable to generate predictions at this time. Please check your internet connection and try again later.</p>
            </div>
        </div>
    `;
}

// ========== Initialization ==========
function initializePredictionModule() {
    console.log('[Prediction] Initializing prediction module...');
    
    // Run initial prediction
    runPredictionAnalysis();
    
    // Set up periodic updates
    setInterval(runPredictionAnalysis, PREDICTION_CONFIG.UPDATE_INTERVAL);
    
    console.log('[Prediction] Module initialized. Updates every', 
                PREDICTION_CONFIG.UPDATE_INTERVAL / 1000 / 60, 'minutes');
}

// Auto-initialize when DOM is ready
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initializePredictionModule);
} else {
    initializePredictionModule();
}
