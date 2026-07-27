from PIL import Image
import os

def resize_image(input_path, max_width=200, quality=75):
    with Image.open(input_path) as img:
        # Calculate the new height to maintain aspect ratio
        width, height = img.size
        new_height = int((max_width / width) * height)
        
        # Resize the image using LANCZOS for high-quality downscaling
        img = img.resize((max_width, new_height), Image.Resampling.LANCZOS)
        
        # Overwrite the original image with the resized one
        img.save(input_path, 'JPEG', quality=quality)

def process_images():
    current_directory = os.path.dirname(os.path.abspath(__file__))  # Get the directory where the script is located
    
    for filename in os.listdir(current_directory):
        if filename.lower().endswith('.jpg') or filename.lower().endswith('.jpeg'):
            input_path = os.path.join(current_directory, filename)
            resize_image(input_path)
            print(f"Resized: {filename}")

if __name__ == "__main__":
    process_images()
