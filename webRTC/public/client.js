// DOM elements.
const roomSelectionContainer = document.getElementById('room-selection-container')
const roomInput = document.getElementById('room-input')
const connectButton = document.getElementById('connect-button')

const videoChatContainer = document.getElementById('video-chat-container')
const localVideoComponent = document.getElementById('local-video')
const remoteVideoComponent = document.getElementById('remote-video')

// Variables.
const socket = io()
let mediaConstraints = {
  audio: false,
  video: {
    width: { ideal: 1920, max: 1920 },
    height: { ideal: 1080, max: 1080 },
    frameRate: { ideal: 30, max: 60 }
  }
};

let localStream
let remoteStream
let isRoomCreator
let rtcPeerConnection // Connection between the local device and the remote peer.
let roomId

// High-resolution measurement variables
let frameTimestamps = new Map();
let rttMeasurements = [];
let frameDelayStats = [];
let customRTTMeasurements = [];
let pingTimers = new Map();
const MAX_SAMPLES = 1000; // Store up to 1000 samples for the graph

// Free public STUN servers provided by Google.
const iceServers = {
  iceServers: [
    { urls: 'stun:stun.l.google.com:19302' },
    { urls: 'stun:stun1.l.google.com:19302' },
    { urls: 'stun:stun2.l.google.com:19302' },
    { urls: 'stun:stun3.l.google.com:19302' },
    { urls: 'stun:stun4.l.google.com:19302' },
  ],
  // Add ICE transport constraints
  iceTransportPolicy: 'all', //'all'// Use all candidates but prefer IPv4
  // Optional: specify which candidate types to gather
  iceCandidatePoolSize: 10
}

// Canvas for RTT graph
let rttCanvas, rttCtx;

// Monitoring intervals
let monitoringIntervals = [];
let pingChannel = null;

// Bitrate monitoring
let bitrateHistory = [];
let lastBytesSent = 0;
let lastTimestamp = 0;

// BUTTON LISTENER ============================================================
connectButton.addEventListener('click', () => {
  const enableAudio = document.getElementById('enable-audio-checkbox').checked;
  mediaConstraints.audio = enableAudio;
  joinRoom(roomInput.value);
});

// SOCKET EVENT CALLBACKS =====================================================
socket.on('room_created', async () => {
  console.log('Socket event callback: room_created')

  await setLocalStream(mediaConstraints)
  isRoomCreator = true
})

socket.on('room_joined', async () => {
  console.log('Socket event callback: room_joined')

  await setLocalStream(mediaConstraints)
  socket.emit('start_call', roomId)
})

socket.on('full_room', () => {
  console.log('Socket event callback: full_room')

  alert('The room is full, please try another one')
})

socket.on('start_call', async () => {
  console.log('Socket event callback: start_call')

  if (isRoomCreator) {
    rtcPeerConnection = new RTCPeerConnection(iceServers)
    addLocalTracks(rtcPeerConnection)
    setupDataChannelHandlers()
    rtcPeerConnection.ontrack = setRemoteStream
    rtcPeerConnection.onicecandidate = sendIceCandidate
    
    await createOffer(rtcPeerConnection)
    startWebRTCStatsMonitoring()
    enableDeepWebRTCDebugging()
    startBitrateMonitoring()
    analyzeSDP()
  }
})

socket.on('webrtc_offer', async (event) => {
  console.log('Socket event callback: webrtc_offer')

  if (!isRoomCreator) {
    rtcPeerConnection = new RTCPeerConnection(iceServers)
    addLocalTracks(rtcPeerConnection)
    setupDataChannelHandlers()
    rtcPeerConnection.ontrack = setRemoteStream
    rtcPeerConnection.onicecandidate = sendIceCandidate
    rtcPeerConnection.setRemoteDescription(new RTCSessionDescription(event))
    
    await createAnswer(rtcPeerConnection)
    startWebRTCStatsMonitoring()
    enableDeepWebRTCDebugging()
    startBitrateMonitoring()
    analyzeSDP()
  }
})

socket.on('webrtc_answer', (event) => {
  console.log('Socket event callback: webrtc_answer')

  rtcPeerConnection.setRemoteDescription(new RTCSessionDescription(event))
})

socket.on('webrtc_ice_candidate', (event) => {
  console.log('Socket event callback: webrtc_ice_candidate')

  // ICE candidate configuration.
  const candidate = new RTCIceCandidate({
    sdpMLineIndex: event.label,
    candidate: event.candidate,
  })
  rtcPeerConnection.addIceCandidate(candidate)
})

// FUNCTIONS ==================================================================
function joinRoom(room) {
  if (room === '') {
    alert('Please type a room ID')
  } else {
    roomId = room
    socket.emit('join', room)
    showVideoConference()
  }
}

function showVideoConference() {
  roomSelectionContainer.style.display = 'none'
  videoChatContainer.style.display = 'block'
  initializeRTTGraph();
}

async function setLocalStream(mediaConstraints) {
  try {
    localStream = await navigator.mediaDevices.getUserMedia(mediaConstraints)
    localVideoComponent.srcObject = localStream
    setupFrameDelayMonitoring()
  } catch (error) {
    console.error('Could not get user media', error)
  }
}

function addLocalTracks(rtcPeerConnection) {
  localStream.getTracks().forEach((track) => {
    rtcPeerConnection.addTrack(track, localStream)
  })
}

async function createOffer(rtcPeerConnection) {
  try {
    const sessionDescription = await rtcPeerConnection.createOffer()
    rtcPeerConnection.setLocalDescription(sessionDescription)
    
    socket.emit('webrtc_offer', {
      type: 'webrtc_offer',
      sdp: sessionDescription,
      roomId,
    })
  } catch (error) {
    console.error(error)
  }
}

async function createAnswer(rtcPeerConnection) {
  try {
    const sessionDescription = await rtcPeerConnection.createAnswer()
    rtcPeerConnection.setLocalDescription(sessionDescription)
    
    socket.emit('webrtc_answer', {
      type: 'webrtc_answer',
      sdp: sessionDescription,
      roomId,
    })
  } catch (error) {
    console.error(error)
  }
}

function setRemoteStream(event) {
  remoteVideoComponent.srcObject = event.streams[0]
  remoteStream = event.streams[0]
}

function sendIceCandidate(event) {
  if (event.candidate) {
    const candidate = event.candidate.candidate;
    
    // Filter out IPv6 candidates and only send IPv4 candidates
    if (!candidate.includes('udp6') && !candidate.includes('tcptls') && 
        !candidate.includes('UDP') && candidate.includes('udp')) {
      
      console.log('Sending IPv4 candidate:', candidate);
      
      socket.emit('webrtc_ice_candidate', {
        roomId,
        label: event.candidate.sdpMLineIndex,
        candidate: event.candidate.candidate,
      });
    } else {
      console.log('Filtered out non-IPv4 candidate:', candidate);
    }
  }
}

// HIGH-RESOLUTION MEASUREMENT FUNCTIONS ======================================

function setupFrameDelayMonitoring() {
  const videoElement = document.getElementById('local-video');
  
  videoElement.addEventListener('loadedmetadata', () => {
    // Capture frame timestamps using requestVideoFrameCallback
    if ('requestVideoFrameCallback' in videoElement) {
      function captureFrameTimestamp() {
        const now = performance.now();
        frameTimestamps.set(now, now); // Store frame presentation time
        videoElement.requestVideoFrameCallback(captureFrameTimestamp);
      }
      videoElement.requestVideoFrameCallback(captureFrameTimestamp);
    }
  });
}

function setupDataChannelHandlers() {
  if (rtcPeerConnection) {
    rtcPeerConnection.ondatachannel = (event) => {
      const channel = event.channel;
      if (channel.label === 'ping') {
        channel.onmessage = (event) => {
          const data = JSON.parse(event.data);
          if (data.type === 'ping') {
            // Send pong back immediately
            channel.send(JSON.stringify({ 
              type: 'pong', 
              id: data.id 
            }));
          }
        };
      }
    };
  }
}

function startCustomRTTMeasurement() {
  // Create the data channel once
  function ensurePingChannel() {
    // Only the offerer (room creator) should create the data channel. The non-offer side will receive it via ondatachannel.
    if (!isRoomCreator) {
      // If we already adopted a channel from ondatachannel, reuse it
      if (pingChannel && pingChannel.readyState === 'open') return pingChannel;
      return null;
    }

    if (!pingChannel || pingChannel.readyState !== 'open') {
      // Close any previous channel reference safely
      try { if (pingChannel) pingChannel.close(); } catch(e) {}
      pingChannel = rtcPeerConnection.createDataChannel('ping');
      pingChannel.onopen = () => {
        console.log('Ping channel opened');
      };
      pingChannel.onmessage = (event) => {
        const data = JSON.parse(event.data);
        if (data.type === 'pong') {
          const rtt = performance.now() - data.id;
          customRTTMeasurements.push({
            timestamp: performance.now(),
            rtt: rtt
          });
          
          if (customRTTMeasurements.length > MAX_SAMPLES) {
            customRTTMeasurements.shift();
          }
          
          pingTimers.delete(data.id);
        }
      };
    }
    return pingChannel;
  }

  // Send ping every 500ms using the same channel (reduced from 200ms)
  return setInterval(() => {
    if (rtcPeerConnection && rtcPeerConnection.connectionState === 'connected') {
      const channel = ensurePingChannel();
      const pingId = performance.now();
      
      if (channel.readyState === 'open') {
        pingTimers.set(pingId, pingId);
        channel.send(JSON.stringify({ type: 'ping', id: pingId }));
        
        setTimeout(() => {
          if (pingTimers.has(pingId)) {
            pingTimers.delete(pingId);
            // Consider this as lost packet
          }
        }, 1000);
      }
    }
  }, 500); // Increased from 200ms to 500ms
}

function startHighResolutionWebRTCStats() {
  if (!rtcPeerConnection) return null;

  // Use 500ms interval instead of 100ms for better performance
  return setInterval(async () => {
    const stats = await rtcPeerConnection.getStats();
    const now = performance.now();
    
    stats.forEach(report => {
      // High-resolution RTT measurement
      if (report.type === 'remote-inbound-rtp' && report.kind === 'video') {
        const rtt = report.roundTripTime * 1000; // Convert to ms
        if (rtt) {
          rttMeasurements.push({
            timestamp: now,
            rtt: rtt,
            jitter: report.jitter * 1000 // Convert to ms
          });
          
          // Keep only last MAX_SAMPLES measurements
          if (rttMeasurements.length > MAX_SAMPLES) {
            rttMeasurements.shift();
          }
        }
      }
      
      // Frame delay measurement using capture-to-decode timing
      if (report.type === 'inbound-rtp' && report.kind === 'video') {
        const framesDecoded = report.framesDecoded;
        const captureTimeMs = report.estimatedCaptureTime;
        
        if (captureTimeMs && framesDecoded > 0) {
          const captureTime = captureTimeMs;
          const currentTime = now;
          const frameDelay = currentTime - captureTime;
          
          frameDelayStats.push({
            timestamp: now,
            delay: frameDelay,
            frameId: framesDecoded
          });
          
          // Keep only recent measurements
          if (frameDelayStats.length > MAX_SAMPLES) {
            frameDelayStats.shift();
          }
        }
      }
    });
    
    updateHighResDiagnostics();
    drawRTTGraph(); // Update the graph with new data
  }, 500); // Increased from 100ms to 500ms
}

function updateHighResDiagnostics() {
  const output = document.getElementById('diagnostics-output');
  
  // Calculate statistics for the last 1000 samples
  const recentRTT = rttMeasurements.slice(-100);
  const recentCustomRTT = customRTTMeasurements.slice(-100);
  
  const currentRTT = recentRTT.length > 0 ? 
    recentRTT[recentRTT.length - 1].rtt : 0;
  
  const currentCustomRTT = recentCustomRTT.length > 0 ?
    recentCustomRTT[recentCustomRTT.length - 1].rtt : 0;
  
  // Calculate statistics
  const rttValues = recentRTT.map(m => m.rtt);
  const avgRTT = rttValues.length > 0 ? 
    rttValues.reduce((sum, rtt) => sum + rtt, 0) / rttValues.length : 0;
  
  const minRTT = rttValues.length > 0 ? Math.min(...rttValues) : 0;
  const maxRTT = rttValues.length > 0 ? Math.max(...rttValues) : 0;
  
  // Calculate standard deviation
  const variance = rttValues.length > 0 ? 
    rttValues.reduce((sum, rtt) => sum + Math.pow(rtt - avgRTT, 2), 0) / rttValues.length : 0;
  const stdDev = Math.sqrt(variance);
  
  // Calculate packet loss (based on custom ping timeouts)
  const totalPings = recentCustomRTT.length + (pingTimers.size > 5 ? 5 : pingTimers.size); // Estimate
  const packetLoss = totalPings > 0 ? (pingTimers.size / totalPings) * 100 : 0;
  
  let diagnosticsText = `⚡ HIGH-RESOLUTION METRICS (Last 100 samples)\n\n`;
  diagnosticsText += `📊 Network Statistics:\n`;
  diagnosticsText += `Current RTT: ${currentRTT.toFixed(1)} ms\n`;
  diagnosticsText += `Average RTT: ${avgRTT.toFixed(1)} ms\n`;
  diagnosticsText += `Min/Max RTT: ${minRTT.toFixed(1)} / ${maxRTT.toFixed(1)} ms\n`;
  diagnosticsText += `Jitter (Std Dev): ${stdDev.toFixed(1)} ms\n`;
  diagnosticsText += `Packet Loss: ${packetLoss.toFixed(1)}%\n`;
  diagnosticsText += `Samples: ${rttMeasurements.length}/${MAX_SAMPLES}\n\n`;
  
  // Frame delay stats
  const recentFrameDelay = frameDelayStats.slice(-50);
  const currentFrameDelay = recentFrameDelay.length > 0 ?
    recentFrameDelay[recentFrameDelay.length - 1].delay : 0;
  
  const avgFrameDelay = recentFrameDelay.length > 0 ?
    recentFrameDelay.reduce((sum, f) => sum + f.delay, 0) / recentFrameDelay.length : 0;
  
  diagnosticsText += `🎬 Frame Timing:\n`;
  diagnosticsText += `Current Frame Delay: ${currentFrameDelay.toFixed(1)} ms\n`;
  diagnosticsText += `Average Frame Delay: ${avgFrameDelay.toFixed(1)} ms\n\n`;
  
  // Quality indicators
  diagnosticsText += `📈 Quality Indicators:\n`;
  diagnosticsText += `RTT Quality: ${getQualityIndicator(avgRTT, 'rtt')}\n`;
  diagnosticsText += `Jitter Quality: ${getQualityIndicator(stdDev, 'jitter')}\n`;
  diagnosticsText += `Overall: ${getOverallQuality(avgRTT, stdDev, packetLoss)}`;
  
  output.textContent = diagnosticsText;
}

function getQualityIndicator(value, type) {
  if (type === 'rtt') {
    if (value < 50) return '🟢 Excellent';
    if (value < 100) return '🟡 Good';
    if (value < 200) return '🟠 Fair';
    return '🔴 Poor';
  } else if (type === 'jitter') {
    if (value < 10) return '🟢 Excellent';
    if (value < 20) return '🟡 Good';
    if (value < 50) return '🟠 Fair';
    return '🔴 Poor';
  }
  return '⚪ Unknown';
}

function getOverallQuality(avgRTT, jitter, packetLoss) {
  let score = 0;
  if (avgRTT < 100) score += 2;
  else if (avgRTT < 200) score += 1;
  
  if (jitter < 20) score += 2;
  else if (jitter < 50) score += 1;
  
  if (packetLoss < 1) score += 2;
  else if (packetLoss < 5) score += 1;
  
  if (score >= 5) return '🟢 Excellent';
  if (score >= 3) return '🟡 Good';
  if (score >= 1) return '🟠 Fair';
  return '🔴 Poor';
}

// RTT GRAPH FUNCTIONS ========================================================

function initializeRTTGraph() {
  rttCanvas = document.getElementById('rtt-graph');
  rttCtx = rttCanvas.getContext('2d');
  
  // Set canvas size
  rttCanvas.width = rttCanvas.offsetWidth;
  rttCanvas.height = rttCanvas.offsetHeight;
  
  // Draw initial graph
  drawRTTGraph();
}

function drawRTTGraph() {
  if (!rttCtx || rttMeasurements.length < 2) return;
  
  const width = rttCanvas.width;
  const height = rttCanvas.height;
  const padding = 20;
  
  // Clear canvas
  rttCtx.clearRect(0, 0, width, height);
  
  // Get the data to display (last 1000 samples or all available)
  const displayData = rttMeasurements.slice(-MAX_SAMPLES);
  const rttValues = displayData.map(m => m.rtt);
  
  if (rttValues.length < 2) return;
  
  // Calculate graph bounds
  const maxRTT = Math.max(...rttValues) * 1.1; // Add 10% padding
  const minRTT = Math.min(0, Math.min(...rttValues) * 0.9);
  
  // Draw grid and labels
  drawGrid(rttCtx, width, height, padding, minRTT, maxRTT);
  
  // Draw RTT line
  rttCtx.beginPath();
  rttCtx.strokeStyle = '#00ff00';
  rttCtx.lineWidth = 2;
  
  displayData.forEach((measurement, index) => {
    const x = padding + (index / (displayData.length - 1)) * (width - 2 * padding);
    const y = height - padding - ((measurement.rtt - minRTT) / (maxRTT - minRTT)) * (height - 2 * padding);
    
    if (index === 0) {
      rttCtx.moveTo(x, y);
    } else {
      rttCtx.lineTo(x, y);
    }
  });
  
  rttCtx.stroke();
  
  // Draw current value indicator
  if (displayData.length > 0) {
    const currentRTT = displayData[displayData.length - 1].rtt;
    const currentX = width - padding;
    const currentY = height - padding - ((currentRTT - minRTT) / (maxRTT - minRTT)) * (height - 2 * padding);
    
    // Draw point
    rttCtx.beginPath();
    rttCtx.arc(currentX, currentY, 4, 0, 2 * Math.PI);
    rttCtx.fillStyle = '#ff4444';
    rttCtx.fill();
    
    // Draw value text
    rttCtx.fillStyle = '#ff4444';
    rttCtx.font = '12px monospace';
    rttCtx.fillText(`${currentRTT.toFixed(1)} ms`, currentX - 40, currentY - 8);
  }
}

function drawGrid(ctx, width, height, padding, minRTT, maxRTT) {
  // Draw background
  ctx.fillStyle = '#1a1a1a';
  ctx.fillRect(padding, padding, width - 2 * padding, height - 2 * padding);
  
  // Draw grid lines
  ctx.strokeStyle = '#333';
  ctx.lineWidth = 1;
  
  // Horizontal grid lines (RTT values)
  const gridLines = 5;
  for (let i = 0; i <= gridLines; i++) {
    const y = padding + (i / gridLines) * (height - 2 * padding);
    const rttValue = maxRTT - (i / gridLines) * (maxRTT - minRTT);
    
    ctx.beginPath();
    ctx.moveTo(padding, y);
    ctx.lineTo(width - padding, y);
    ctx.stroke();
    
    // Y-axis labels
    ctx.fillStyle = '#888';
    ctx.font = '10px monospace';
    ctx.textAlign = 'right';
    ctx.fillText(`${rttValue.toFixed(0)} ms`, padding - 5, y + 3);
  }
  
  // X-axis label (time)
  ctx.fillStyle = '#888';
  ctx.textAlign = 'center';
  ctx.fillText(`Last ${Math.min(MAX_SAMPLES, rttMeasurements.length)} samples`, width / 2, height - 5);
  
  // Graph title
  ctx.fillStyle = '#fff';
  ctx.font = '12px monospace';
  ctx.textAlign = 'center';
  ctx.fillText('RTT Over Time (ms)', width / 2, 15);
}

// BITRATE MONITORING ========================================================

function startBitrateMonitoring() {
  if (!rtcPeerConnection) return;
  
  return setInterval(async () => {
    const stats = await rtcPeerConnection.getStats();
    let output = '';
    
    stats.forEach(report => {
      // Monitor outbound video stream
      if (report.type === 'outbound-rtp' && report.kind === 'video') {
        const currentBytes = report.bytesSent;
        const currentTime = report.timestamp;
        
        // Calculate instant bitrate
        let instantBitrate = 0;
        if (lastTimestamp > 0) {
          const bytesDiff = currentBytes - lastBytesSent;
          const timeDiff = (currentTime - lastTimestamp) / 1000;
          instantBitrate = (bytesDiff * 8) / timeDiff / 1000;
          
          bitrateHistory.push(instantBitrate);
          if (bitrateHistory.length > 10) bitrateHistory.shift();
          
          const avgBitrate = bitrateHistory.reduce((a, b) => a + b, 0) / bitrateHistory.length;
          
          output += `🎥 VIDEO BITRATE:\n`;
          output += `Instant: ${instantBitrate.toFixed(2)} kbps\n`;
          output += `Average: ${avgBitrate.toFixed(2)} kbps\n`;
          output += `Frames Encoded: ${report.framesEncoded || 0}\n`;
          output += `Packets Sent: ${report.packetsSent}\n\n`;
        }
        
        lastBytesSent = currentBytes;
        lastTimestamp = currentTime;
        
        // Chrome-specific GCC stats
        if (report.googTargetBitrate) {
          output += `🎯 GCC CONTROL:\n`;
          output += `Target Bitrate: ${(parseInt(report.googTargetBitrate) / 1000).toFixed(2)} kbps\n`;
          if (report.googAvailableSendBandwidth) {
            output += `Available Bandwidth: ${(parseInt(report.googAvailableSendBandwidth) / 1000).toFixed(2)} kbps\n`;
          }
          if (report.googBandwidthLimitedResolution) {
            output += `Bandwidth Limited: ${report.googBandwidthLimitedResolution}\n`;
          }
          output += '\n';
        }
      }
      
      // Monitor congestion control state
      if (report.type === 'candidate-pair' && report.state === 'succeeded') {
        output += `📊 NETWORK STATE:\n`;
        output += `RTT: ${(report.currentRoundTripTime * 1000).toFixed(1)} ms\n`;
        output += `Available Bitrate: ${(report.availableOutgoingBitrate / 1000).toFixed(2)} kbps\n`;
      }
    });
    
    // Update display
    const bitrateOutput = document.getElementById('bitrate-output');
    if (bitrateOutput) {
      bitrateOutput.textContent = output;
    }
  }, 1000);
}

// DEEP DEBUGGING FUNCTIONS ==================================================

function enableDeepWebRTCDebugging() {
    if (!rtcPeerConnection) return;
    
    // Monitor ALL stat changes
    return setInterval(async () => {
        const stats = await rtcPeerConnection.getStats();
        let debugOutput = '=== DEEP WEBRTC DEBUG ===\n\n';
        
        stats.forEach(report => {
            debugOutput += `[${report.type}] ${report.id}\n`;
            
            // Outbound video (sender side)
            if (report.type === 'outbound-rtp' && report.kind === 'video') {
                debugOutput += `  SSRC: ${report.ssrc}\n`;
                debugOutput += `  Bytes Sent: ${report.bytesSent}\n`;
                debugOutput += `  Packets Sent: ${report.packetsSent}\n`;
                debugOutput += `  Frames Encoded: ${report.framesEncoded || 'N/A'}\n`;
                debugOutput += `  Frame Width: ${report.frameWidth || 'N/A'}\n`;
                debugOutput += `  Frame Height: ${report.frameHeight || 'N/A'}\n`;
                
                // Chrome-specific GCC stats
                const gccStats = [
                    'googTargetBitrate', 'googAvailableSendBandwidth',
                    'googTransmitBitrate', 'googRetransmitBitrate',
                    'googBandwidthLimitedResolution', 'googCPULimitedResolution'
                ];
                
                gccStats.forEach(stat => {
                    if (report[stat] !== undefined) {
                        debugOutput += `  ${stat}: ${report[stat]}\n`;
                    }
                });
                debugOutput += '\n';
            }
            
            // Remote inbound (receiver side - your fake RR destination)
            if (report.type === 'remote-inbound-rtp' && report.kind === 'video') {
                debugOutput += `  📊 REMOTE INBOUND (FAKE RR TARGET):\n`;
                debugOutput += `  SSRC: ${report.ssrc}\n`;
                debugOutput += `  Fraction Lost: ${report.fractionLost} (${(report.fractionLost * 100).toFixed(1)}%)\n`;
                debugOutput += `  Jitter: ${report.jitter}\n`;
                debugOutput += `  Round Trip Time: ${report.roundTripTime}\n`;
                debugOutput += `  Total RTT: ${report.totalRoundTripTime}\n`;
                debugOutput += `  RTT Measurements: ${report.roundTripTimeMeasurements}\n`;
                debugOutput += '\n';
            }
            
            // Inbound video (receiver side)
            if (report.type === 'inbound-rtp' && report.kind === 'video') {
                debugOutput += `  📥 INBOUND VIDEO:\n`;
                debugOutput += `  SSRC: ${report.ssrc}\n`;
                debugOutput += `  Bytes Received: ${report.bytesReceived}\n`;
                debugOutput += `  Packets Received: ${report.packetsReceived}\n`;
                debugOutput += `  Packets Lost: ${report.packetsLost}\n`;
                debugOutput += `  Frames Decoded: ${report.framesDecoded || 'N/A'}\n`;
                debugOutput += `  Frame Width: ${report.frameWidth || 'N/A'}\n`;
                debugOutput += `  Frame Height: ${report.frameHeight || 'N/A'}\n`;
                debugOutput += '\n';
            }
            
            // Candidate pair (network info)
            if (report.type === 'candidate-pair' && report.state === 'succeeded') {
                debugOutput += `  🌐 NETWORK:\n`;
                debugOutput += `  State: ${report.state}\n`;
                debugOutput += `  RTT: ${report.currentRoundTripTime}\n`;
                debugOutput += `  Available Outgoing Bitrate: ${report.availableOutgoingBitrate}\n`;
                debugOutput += `  Bytes Sent: ${report.bytesSent}\n`;
                debugOutput += `  Bytes Received: ${report.bytesReceived}\n`;
                debugOutput += '\n';
            }
            
            // Track stats
            if (report.type === 'track') {
                debugOutput += `  🎬 TRACK: ${report.trackIdentifier}\n`;
                debugOutput += `  Frame Width: ${report.frameWidth}\n`;
                debugOutput += `  Frame Height: ${report.frameHeight}\n`;
                debugOutput += `  Frames Per Second: ${report.framesPerSecond}\n`;
                debugOutput += '\n';
            }
        });
        
        // Update debug display
        const debugElement = document.getElementById('deep-debug-output');
        if (debugElement) {
            debugElement.textContent = debugOutput;
        }
    }, 2000);
}

// CODEC AND HARDWARE ANALYSIS ==============================================

function checkCodecAndHardware() {
    if (!rtcPeerConnection) return;
    
    return setInterval(async () => {
        const stats = await rtcPeerConnection.getStats();
        let codecInfo = '=== CODEC & HARDWARE ANALYSIS ===\n\n';
        
        stats.forEach(report => {
            // Check codec specifications
            if (report.type === 'codec') {
                codecInfo += `🔧 CODEC: ${report.mimeType}\n`;
                codecInfo += `  Payload Type: ${report.payloadType}\n`;
                codecInfo += `  Clock Rate: ${report.clockRate}\n`;
                codecInfo += `  Channels: ${report.channels || 1}\n`;
                codecInfo += `  SDP FMTP: ${report.sdpFmtpLine || 'N/A'}\n`;
                
                // Check for fixed bitrate parameters
                if (report.sdpFmtpLine) {
                    const fmtp = report.sdpFmtpLine.toLowerCase();
                    if (fmtp.includes('max-bitrate') || fmtp.includes('bitrate') || fmtp.includes('maxbr')) {
                        codecInfo += `  🚨 FIXED BITRATE PARAMETER DETECTED\n`;
                    }
                }
                codecInfo += '\n';
            }
            
            // Check if there are bandwidth constraints in SDP
            if (report.type === 'outbound-rtp' && report.kind === 'video') {
                codecInfo += `🎥 VIDEO STREAM:\n`;
                codecInfo += `  Codec: ${report.codecId || 'Unknown'}\n`;
                codecInfo += `  Quality Limitation: ${report.qualityLimitationReason || 'none'}\n`;
                codecInfo += `  Quality Limitation Resolutions: ${report.qualityLimitationResolutionChanges || 0}\n`;
                
                // Hardware encoding detection
                codecInfo += `  Encoder: ${report.encoderImplementation || 'Unknown'}\n`;
                codecInfo += `  Power Efficient: ${report.powerEfficientEncoder !== undefined ? report.powerEfficientEncoder : 'N/A'}\n`;
                
                // Hardware encoders often have fixed bitrate modes
                if (report.encoderImplementation && 
                    (report.encoderImplementation.includes('Hardware') || 
                     report.encoderImplementation.includes('VAAPI') ||
                     report.encoderImplementation.includes('NVENC') ||
                     report.encoderImplementation.includes('MediaFoundation'))) {
                    codecInfo += '  ⚠️  HARDWARE ENCODER DETECTED - may have fixed bitrate modes\n';
                }
                
                // Check for fixed bitrate indicators
                if (report.googTargetBitrate) {
                    const targetBitrate = parseInt(report.googTargetBitrate) / 1000;
                    codecInfo += `  Target Bitrate: ${targetBitrate.toFixed(2)} kbps\n`;
                    
                    // Detect fixed bitrate patterns
                    if (targetBitrate > 0 && targetBitrate < 500) {
                        codecInfo += `  ⚠️  LOW FIXED BITRATE SUSPECTED\n`;
                    }
                }
                codecInfo += '\n';
            }
        });
        
        // Update codec analysis display
        const codecElement = document.getElementById('codec-analysis-output');
        if (codecElement) {
            codecElement.textContent = codecInfo;
        }
    }, 3000);
}

// SDP ANALYSIS =============================================================

function analyzeSDP() {
    if (!rtcPeerConnection) return;
    
    // Analyze local SDP
    rtcPeerConnection.getLocalDescription().then(localDesc => {
        if (localDesc) {
            console.log('=== LOCAL SDP ANALYSIS ===');
            analyzeSDPCodecs(localDesc.sdp, 'local');
        }
    });
    
    // Analyze remote SDP
    rtcPeerConnection.getRemoteDescription().then(remoteDesc => {
        if (remoteDesc) {
            console.log('=== REMOTE SDP ANALYSIS ==='); 
            analyzeSDPCodecs(remoteDesc.sdp, 'remote');
        }
    });
}

function analyzeSDPCodecs(sdp, type) {
    const lines = sdp.split('\n');
    let inVideoSection = false;
    let sdpAnalysis = `=== ${type.toUpperCase()} SDP ANALYSIS ===\n\n`;
    
    lines.forEach(line => {
        if (line.startsWith('m=video')) {
            inVideoSection = true;
            sdpAnalysis += `🎥 Video media line: ${line}\n`;
        } else if (line.startsWith('m=') && !line.startsWith('m=video')) {
            inVideoSection = false;
        }
        
        if (inVideoSection) {
            // Check for codec lines
            if (line.startsWith('a=rtpmap:')) {
                sdpAnalysis += `  Codec mapping: ${line}\n`;
            }
            // Check for bandwidth constraints
            if (line.startsWith('b=')) {
                sdpAnalysis += `  Bandwidth constraint: ${line}\n`;
                if (line.includes('AS:') || line.includes('TIAS:')) {
                    sdpAnalysis += `  🚨 EXPLICIT BANDWIDTH LIMIT DETECTED\n`;
                }
            }
            // Check for codec parameters
            if (line.startsWith('a=fmtp:')) {
                sdpAnalysis += `  Codec parameters: ${line}\n`;
                
                // Look for fixed bitrate indicators
                if (line.includes('max-bitrate') || line.includes('bitrate') || line.includes('maxbr')) {
                    sdpAnalysis += `  🚨 FIXED BITRATE PARAMETER DETECTED\n`;
                }
            }
        }
    });
    
    console.log(sdpAnalysis);
}

// MAIN STATS MONITORING ====================================================

let statsInterval = null

function startWebRTCStatsMonitoring() {
  if (!rtcPeerConnection) return

  // Clear any existing intervals first
  stopWebRTCStatsMonitoring();

  // Start all monitoring systems
  monitoringIntervals.push(startHighResolutionWebRTCStats());
  monitoringIntervals.push(startCustomRTTMeasurement());
  monitoringIntervals.push(startBitrateMonitoring());
  monitoringIntervals.push(enableDeepWebRTCDebugging());
  monitoringIntervals.push(checkCodecAndHardware());

  // Standard stats (less frequent)
  monitoringIntervals.push(setInterval(async () => {
    const stats = await rtcPeerConnection.getStats()
    let output = ''
    let chromeGccOutput = ''

    stats.forEach(report => {
      if (report.type === 'inbound-rtp' && report.kind === 'video') {
        // existing inbound stats...
      }

      if (report.type === 'outbound-rtp' && report.kind === 'video') {
        const currentBytesSent = report.bytesSent
        const currentTime = report.timestamp

        let outboundBitrateKbps = 0
        if (lastTimestamp) {
          const bytesDiff = currentBytesSent - lastBytesSent
          const timeDiffSec = (currentTime - lastTimestamp) / 1000
          outboundBitrateKbps = (bytesDiff * 8) / 1000 / timeDiffSec
        }

        output += `📤 Outbound Video\n`
        output += `Bitrate: ${outboundBitrateKbps.toFixed(2)} kbps\n`
        output += `Packets Sent: ${report.packetsSent}\n`
        output += `Frames Encoded: ${report.framesEncoded}\n\n`

        lastBytesSent = currentBytesSent
        lastTimestamp = currentTime

        // ➕ Chrome-only GCC internals
        if ('googTargetBitrate' in report || 'googAvailableSendBandwidth' in report) {
          chromeGccOutput += `📊 Chrome GCC Internals\n`
          if (report.googTargetBitrate)
            chromeGccOutput += `Target Bitrate: ${(parseInt(report.googTargetBitrate) / 1000).toFixed(2)} kbps\n`
          if (report.googAvailableSendBandwidth)
            chromeGccOutput += `Available Send Bandwidth: ${(parseInt(report.googAvailableSendBandwidth) / 1000).toFixed(2)} kbps\n`
          if (report.googRetransmitBitrate)
            chromeGccOutput += `Retransmit Bitrate: ${(parseInt(report.googRetransmitBitrate) / 1000).toFixed(2)} kbps\n`
          if (report.googTransmitBitrate)
            chromeGccOutput += `Transmit Bitrate: ${(parseInt(report.googTransmitBitrate) / 1000).toFixed(2)} kbps\n`
          if (report.googBandwidthLimitedResolution)
            chromeGccOutput += `Bandwidth Limited Resolution: ${report.googBandwidthLimitedResolution}\n`
          chromeGccOutput += '\n'
        }
      }

      if (report.type === 'candidate-pair' && report.state === 'succeeded') {
        output += `🔗 Connection Info\n`
        output += `RTT: ${report.currentRoundTripTime.toFixed(3)} sec\n`
        output += `Available Outgoing Bitrate: ${(report.availableOutgoingBitrate / 1000).toFixed(2)} kbps\n`
      }
    })

    // Update the standard diagnostics display
    const standardOutput = document.getElementById('standard-diagnostics-output');
    if (standardOutput) {
      standardOutput.textContent = output;
    }
    
    const chromeGccElement = document.getElementById('chrome-gcc-output');
    if (chromeGccElement) {
      chromeGccElement.textContent = chromeGccOutput || 'Not available in this browser.';
    }
  }, 1000))
}

function stopWebRTCStatsMonitoring() {
  monitoringIntervals.forEach(interval => {
    if (interval) clearInterval(interval);
  });
  monitoringIntervals = [];
  
  // Reset ping channel
  pingChannel = null;
}

// Handle window resize
window.addEventListener('resize', () => {
  if (rttCanvas) {
    rttCanvas.width = rttCanvas.offsetWidth;
    rttCanvas.height = rttCanvas.offsetHeight;
    drawRTTGraph();
  }
});

// Clean up when leaving the page
window.addEventListener('beforeunload', () => {
  stopWebRTCStatsMonitoring();
});

// Browser console diagnostics
window.diagnoseBitrateAdaptation = function() {
    console.log('=== BITRATE ADAPTATION DIAGNOSIS ===');
    
    // 1. Check SDP for constraints
    analyzeSDP();
    
    // 2. Check current stats
    setTimeout(() => {
        console.log('Running codec and hardware analysis...');
    }, 1000);
    
    // 3. Check if we can manually control bitrate
    setTimeout(() => {
        console.log('Diagnosis complete. Check the Deep Debug section for details.');
    }, 2000);
};