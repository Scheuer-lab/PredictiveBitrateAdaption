const fs = require('fs')
const https = require('https')
const express = require('express')
const { Server } = require('socket.io')

const app = express()

const serverOptions = {
  key: fs.readFileSync('192.168.1.2-key.pem'),
  cert: fs.readFileSync('192.168.1.2.pem'),
}

const httpsServer = https.createServer(serverOptions, app)
const io = new Server(httpsServer)

app.use('/', express.static('public'))

// Enhanced server logging
io.on('connection', (socket) => {
  console.log(`🔌 [${new Date().toISOString()}] New client connected: ${socket.id}`)
  
  socket.on('join', (roomId) => {
    console.log(`🚪 [${new Date().toISOString()}] Client ${socket.id} joining room: ${roomId}`)
    const selectedRoom = io.sockets.adapter.rooms.get(roomId)
    const numberOfClients = selectedRoom ? selectedRoom.size : 0
    console.log(`📊 [${new Date().toISOString()}] Room ${roomId} currently has ${numberOfClients} clients`)
    
    if (numberOfClients === 0) {
      console.log(`Creating room ${roomId} and emitting room_created socket event`)
      socket.join(roomId)
      socket.emit('room_created', roomId)
    } else if (numberOfClients === 1) {
      console.log(`Joining room ${roomId} and emitting room_joined socket event`)
      socket.join(roomId)
      socket.emit('room_joined', roomId)
    } else {
      console.log(`Can't join room ${roomId}, emitting full_room socket event`)
      socket.emit('full_room', roomId)
    }
  })

  socket.on('disconnect', (reason) => {
    console.log(`🔌 [${new Date().toISOString()}] Client ${socket.id} disconnected: ${reason}`)
  })

  // Log all WebRTC events with timestamps
  socket.on('start_call', (roomId) => {
    console.log(`📞 [${new Date().toISOString()}] START_CALL from ${socket.id} in room ${roomId}`)
    socket.to(roomId).emit('start_call')
  })

  socket.on('webrtc_offer', (event) => {
    console.log(`📞 [${new Date().toISOString()}] WEBRTC_OFFER from ${socket.id} in room ${event.roomId}`)
    console.log(`   SDP type: ${event.sdp.type}, length: ${event.sdp.sdp.length} chars`)
    socket.to(event.roomId).emit('webrtc_offer', event.sdp)
  })

  socket.on('webrtc_answer', (event) => {
    console.log(`📞 [${new Date().toISOString()}] WEBRTC_ANSWER from ${socket.id} in room ${event.roomId}`)
    console.log(`   SDP type: ${event.sdp.type}, length: ${event.sdp.sdp.length} chars`)
    socket.to(event.roomId).emit('webrtc_answer', event.sdp)
  })

  socket.on('webrtc_ice_candidate', (event) => {
    console.log(`📞 [${new Date().toISOString()}] WEBRTC_ICE_CANDIDATE from ${socket.id} in room ${event.roomId}`)
    socket.to(event.roomId).emit('webrtc_ice_candidate', event)
  })
})

// START THE SERVER =================================================================
const port = 3000
httpsServer.listen(port, () => {
  console.log(`HTTPS server running at <host-ip>:${port}`)
})