import struct

#copy TEX data into TRS
track_face_format = '4H 3h 2B I' 
track_texture_format = '2B'  

track_face_size = struct.calcsize(track_face_format) 
track_texture_size = struct.calcsize(track_texture_format) 

def update_track_face(track_face_file, track_texture_file, output_file):
    with open(track_face_file, 'rb+') as face_file, open(track_texture_file, 'rb') as texture_file:
        face_data = face_file.read()  
        texture_data = texture_file.read()
        
        num_faces = len(face_data) // track_face_size
        num_textures = len(texture_data) // track_texture_size
        updated_faces = bytearray(face_data)
        
        for i in range(num_faces):
            face_offset = i * track_face_size
            track_face = struct.unpack_from(track_face_format, face_data, face_offset)
            
            texture_offset = i * track_texture_size
            track_texture = struct.unpack_from(track_texture_format, texture_data, texture_offset)            
 
            indices = track_face[:4]
            normalx, normaly, normalz = track_face[4:7]
            _, _ = track_face[7], track_face[8] 
            color = track_face[9]
            
            new_tile, new_flags = track_texture            
            new_track_face = indices + (normalx, normaly, normalz, new_tile, new_flags, color)            
            struct.pack_into(track_face_format, updated_faces, face_offset, *new_track_face)
        
    with open(output_file, 'wb') as output:
        output.write(updated_faces)

update_track_face('track.trf', 'track.tex', 'track.trf')
