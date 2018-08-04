import numpy as np
import queue
import os
import glob
from threading import Thread
import imageio
import time

# DATA_DIR = './origin-data/road-train-2+valid.v2/train'
DATA_DIR = './contest_data/train_data'


class ImageLoader(object):
    def __init__(self, buffer_size=200, num_slices=1, shuffle=False,
                 expand_y=False, data_dir=DATA_DIR):
        """ Initialize Data Loader
        Args:
            buffer_size (int): Maxium number of items held in queue.
            num_slices (int): If larger than one, slices the image in to n*n grids
            shuffle (boolean): If True, shuffle input file order.
            expand_y (boolean): If False, y will have shape [width, height]
                If False, y will be expanded to [width, height, 1].
            data_dir (string): Directory to read images from.

        """
        self.expand_y = expand_y
        self.img_files = glob.glob(
            os.path.join(data_dir, '*tif*')
        )
        self.img_files = sorted(self.img_files)
        self.mask_files = glob.glob(
            os.path.join(data_dir, '*bmp*')
        )
        self.mask_files = sorted(self.mask_files)

        print('Number of images: {}'.format(len(self.img_files)))
        print('Number of filters: {}'.format(len(self.mask_files)))
        assert len(self.img_files) == len(self.mask_files)

        if shuffle:
            tmp_order = np.arange(len(self.img_files))
            np.random.shuffle(tmp_order)
            self.img_files = [self.img_files[i] for i in tmp_order]
            self.mask_files = [self.mask_files[i] for i in tmp_order]

        self.img_iter = self._iter_list(self.img_files)
        self.mask_iter = self._iter_list(self.mask_files)

        self.num_slices = num_slices
        self.data_pipeline = queue.Queue(maxsize=buffer_size)
        self.eof = False
        self.worker = Thread(target=self._fetch_data)
        self.worker.setDaemon(True)
        self.worker.start()

    def _iter_list(self, file_list):
        """ Helper function to construct a iterable file generator """
        for file in file_list:
            yield file

    def _grid_slice(self, array_in, num_slice):
        """ Helper function to slice input image into n*n grids """
        v_slices = np.split(array_in, num_slice)
        frags = [
            frag for v_slice in v_slices
            for frag in np.split(v_slice, num_slice, axis=1)
        ]

        return frags

    def _fetch_data(self):
        """ Helper function to fetch next image file and put into queue """
        while not self.eof:
            try:
                img_name = next(self.img_iter)
                img = imageio.imread(img_name)
                # 0-1 float32
                img = img.astype(np.float32) / 255.0
                mask = imageio.imread(next(self.mask_iter))
                # 0/1 int8
                mask = (mask[:, :, 0] > 128).astype(np.int8)
                if self.expand_y:
                    mask = np.expand_dims(mask, -1)
                if self.num_slices == 1:
                    self.data_pipeline.put((img, mask))
                else:
                    imgs = self._grid_slice(img, self.num_slices)
                    masks = self._grid_slice(mask, self.num_slices)
                    for img, mask in zip(imgs, masks):
                        self.data_pipeline.put((img, mask))
                # print('fetched {}'.format(img_name))
            except StopIteration:
                self.eof = True

    def finished(self):
        """ Check if all data has been served """
        return self.eof and self.data_pipeline.empty()

    def serve_data(self, num_samples):
        """ Server data from queue
        Args:
            num_samples (int): the number of samples per serve
        Returns:
            x_list, y_list (list): contains np.array as input samples.
        """
        x_list = []
        y_list = []
        while num_samples > 0:
            if self.eof and self.data_pipeline.empty():
                break
            (img, mask) = self.data_pipeline.get()
            x_list.append(img)
            y_list.append(mask)
            self.data_pipeline.task_done()
            num_samples -= 1
        return x_list, y_list

    def join(self):
        """ Join data queue """
        self.data_pipeline.join()


def dummy_consumer(img, mask):
    print('Dummy Consumer Working...')
    print(img.shape, img.dtype)
    print(mask.shape, mask.dtype)


if __name__ == '__main__':
    # unit test
    image_loader = ImageLoader(shuffle=True)
    img_list, mask_list = image_loader.serve_data(5)
    for i in range(5):
        imageio.imwrite('test_img_{}.png'.format(i), img_list[i])
        imageio.imwrite('test_mask_{}.png'.format(i), mask_list[i])
    '''
    while not image_loader.eof:
        img_list, mask_list = image_loader.serve_data(40)
        for (img, mask) in zip(img_list, mask_list):
            dummy_consumer(img, mask)
    image_loader.join()
    '''

