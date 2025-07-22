import os
import json

# Define the base directory where the media files are stored
base_dir = os.path.dirname(os.path.realpath(__file__))

# Define paths to the specific media directories
media_dirs = {
    'movies': os.path.join(base_dir, 'Movies'),
    'shows': os.path.join(base_dir, 'Shows'),
    'books': os.path.join(base_dir, 'Books'),
    'music': os.path.join(base_dir, 'Music')
}

# Placeholder image location
placeholder_image = "placeholder.jpg"

# Function to scan a directory and return the filenames (ignoring hidden files)
def scan_directory(dir_path):
    return [f for f in os.listdir(dir_path) if not f.startswith('.')]

# Function to check if an image exists for a media item, otherwise return the placeholder image
def get_cover_image(media_type, media_name):
    cover_path = os.path.join(media_dirs[media_type.lower()], f"{media_name}.jpg")
    if os.path.exists(cover_path):
        return f"{media_type}/{media_name}.jpg"
    else:
        return placeholder_image

# Function to generate movie data
def generate_movies():
    movie_files = scan_directory(media_dirs['movies'])
    return [{
        'name': os.path.splitext(file)[0],
        'cover': get_cover_image('movies', os.path.splitext(file)[0]),
        'file': f"Movies/{file}"
    } for file in movie_files if file.endswith(('.mp4', '.mkv'))]

# Function to generate show data
def generate_shows():
    show_folders = [f for f in scan_directory(media_dirs['shows']) if os.path.isdir(os.path.join(media_dirs['shows'], f))]
    show_data = []
    for show_folder in show_folders:
        show_cover = get_cover_image('shows', show_folder)
        episodes = [file for file in scan_directory(os.path.join(media_dirs['shows'], show_folder)) if file.endswith(('.mp4', '.mkv'))]
        episodes_data = [{
            'name': episode,
            'file': f"Shows/{show_folder}/{episode}"
        } for episode in episodes]
        show_data.append({
            'name': show_folder,
            'cover': show_cover,
            'episodes': episodes_data
        })
    return show_data

# Function to generate book data
def generate_books():
    book_files = scan_directory(media_dirs['books'])
    return [{
        'name': os.path.splitext(file)[0],
        'cover': get_cover_image('books', os.path.splitext(file)[0]),
        'file': f"Books/{file}"
    } for file in book_files if file.endswith(('.pdf', '.epub'))]

# Function to generate music data (includes covers, but they are not needed and dont show up in the current frontend)
def generate_music():
    music_files = scan_directory(media_dirs['music'])
    return [{
        'name': os.path.splitext(file)[0],
        'cover': get_cover_image('music', os.path.splitext(file)[0]),
        'file': f"Music/{file}"
    } for file in music_files if file.endswith('.mp3')]

# Main function to generate the full media JSON
def generate_media_json():
    media_json = {
        'movies': generate_movies(),
        'shows': generate_shows(),
        'books': generate_books(),
        'music': generate_music()
    }

    # Save the media JSON in the same directory as the script
    with open(os.path.join(base_dir, 'media.json'), 'w', encoding='utf-8') as f:
        json.dump(media_json, f, ensure_ascii=False, indent=4)
    print('media.json has been generated!')

# Run the script to generate the media.json
if __name__ == "__main__":
    generate_media_json()
