#include "optimizer.hpp"

#include <iostream>

using std::vector;

using cv::Mat;
using cv::Range;
using cv::Size;

static const int CROP_ANGLE = 180;
static const int H_FOCUS_ANGLE = 30;
static const int V_FOCUS_ANGLE = 30;
static const int BLUR_FACTOR = 3;

OptimizedImage::OptimizedImage(const cv::Mat& focused,
    const cv::Mat& blurred,
    int focusRow, int focusCol,
    Size fullSize, int leftBuffer) {
  this->focused = Mat(focused);
  this->blurred = Mat(blurred);
  this->focusRow = focusRow;
  this->focusCol = focusCol;
  this->fullSize = fullSize;
  this->leftBuffer = leftBuffer;
}

size_t OptimizedImage::size() const {
  return ImageUtil::imageSize(focused) +
    ImageUtil::imageSize(blurred);
}

static inline int constrainAngle(int x) {
  x %= 360;
  if (x < 0)
    x += 360;
  ENSURES(0 <= x && x < 360);
  return x;
}

static inline double constrainAngle(double x) {
  // https://stackoverflow.com/questions/11498169/dealing-with-angle-wrap-in-c-code
  x = fmod(x,360);
  if (x < 0)
    x += 360;
  ENSURES(0 <= x && x < 360);
  return x;
}

static inline int clamp(int x, int lo, int hi) {
  return std::max(lo, std::min(x, hi));
}

static Mat cropHorizontallyWrapped(const Mat& image, const int leftCol, const int rightCol) {
  REQUIRES(0 <= leftCol && leftCol < image.cols);
  REQUIRES(0 <= rightCol && rightCol < image.cols);

  Mat cropped;
  if (leftCol < rightCol) {
    // Cropped window doesn't wrap around, simple case.
    cropped = Mat(image, Range::all(), Range(leftCol, rightCol));
  } else {
    ASSERT(leftCol > rightCol);
    // Cropped window *does* wrap around. We need to get the part
    // before and after it wraps, which are in two separate places
    // on the image.
    Mat leftMat = Mat(image, Range::all(), Range(leftCol, image.cols));
    Mat rightMat = Mat(image, Range::all(), Range(0, rightCol));
    ImageUtil::hconcat2(leftMat, rightMat, cropped);
  }
  return cropped;
}

OptimizedImage Optimizer::optimizeImage(const Mat& image,
    int angle, int vAngle) {
  Timer timer;

  angle = constrainAngle(angle);
  REQUIRES(H_FOCUS_ANGLE < CROP_ANGLE);

  const int width = image.cols;
  const int height = image.rows;
  const double angleToWidth = width / 360.0;
  const double angleToHeight = height / 180.0;

  int leftAngle = constrainAngle(angle - CROP_ANGLE / 2);
  int rightAngle = constrainAngle(angle + CROP_ANGLE / 2);

  int leftCol = leftAngle * angleToWidth;
  int rightCol = rightAngle * angleToWidth;
  ASSERT(0 <= leftCol);
  ASSERT(leftCol < width);
  ASSERT(0 <= rightCol);
  ASSERT(rightCol < width);

  timer.start();
  Mat cropped = cropHorizontallyWrapped(image, leftCol, rightCol);
  timer.stop("Cropping");

  int focusWidth = H_FOCUS_ANGLE * angleToWidth;

  int focusLeftCol = cropped.cols / 2 - focusWidth / 2;
  int focusRightCol = cropped.cols / 2 + focusWidth / 2;

  timer.start();
  ASSERT(0 <= focusLeftCol);
  ASSERT(focusLeftCol <= focusRightCol);
  ASSERT(focusRightCol < cropped.cols);
  Mat middle = Mat(cropped, Range::all(), Range(focusLeftCol, focusRightCol));
  timer.stop("Splitting (H)");

  int focusHeight = V_FOCUS_ANGLE * angleToHeight;
  int focusMiddleRow = clamp(vAngle * angleToHeight,
      focusHeight / 2, image.rows - focusHeight / 2);
  int focusTopRow = focusMiddleRow - focusHeight / 2;
  int focusBottomRow = focusMiddleRow + focusHeight / 2;

  timer.start();
  Mat focused(middle, Range(focusTopRow, focusBottomRow));
  timer.stop("Splitting (V)");

  Size smallSize(cropped.cols / BLUR_FACTOR, cropped.rows / BLUR_FACTOR);
  Size origSize(cropped.cols, cropped.rows);

  timer.start();
  Mat small, blurred;
  cv::resize(cropped, small, smallSize);
  cv::resize(small, blurred, origSize);
  timer.stop("Blurring");

  OptimizedImage optImage(focused, blurred,
      focusTopRow, focusLeftCol,
      image.size(), leftCol);
  return optImage;
}


static Mat uncropWrapped(const Mat& croppedImage, const int fullWidth, const int leftBuffer) {
  int numRows = croppedImage.size().height;
  int origType = croppedImage.type(); // we use the same type everywhere

  Mat fullLeft, fullCenter, fullRight;

  // Split into 2 cases. In either case, there are 3 images to create
  if (croppedImage.size().width + leftBuffer >= fullWidth) {
    // cropped image wraps around
    // Reconstruct as cropped_right + black + cropped_left

    // cropped right = leftBuffer to end, or in local coords, 0 to width - leftBuffer?
    // cropped left =
    // black = full width - right - left

    int rightEndExclusive = fullWidth - leftBuffer;
    ASSERT(0 <= rightEndExclusive && rightEndExclusive <= croppedImage.cols);
    fullRight = Mat(croppedImage, Range::all(), Range(0, rightEndExclusive));
    fullLeft = Mat(croppedImage, Range::all(), Range(rightEndExclusive, croppedImage.cols));

    int centerCols = fullWidth - fullRight.size().width - fullLeft.size().width;
    ASSERT(centerCols >= 0);

    fullCenter = Mat(numRows, centerCols, origType);
    fullCenter.setTo(cv::Scalar(0,0,0));

  } else {
    // cropped image fully contained
    // Reconstruct as black_left + cropped + black_right

    fullLeft = Mat(numRows, leftBuffer, origType);
    fullCenter = croppedImage;

    int rightBufferCols = fullWidth - leftBuffer - croppedImage.size().width;
    ASSERT(rightBufferCols >= 0);

    fullRight = Mat(numRows, rightBufferCols, origType);

    fullLeft.setTo(cv::Scalar(0,0,0));
    fullRight.setTo(cv::Scalar(0,0,0));
  }


  Mat fullImage;
  ImageUtil::hconcat3(fullLeft, fullCenter, fullRight, fullImage);
  return fullImage;
}

Mat Optimizer::extractImage(const OptimizedImage& optImage) {
  Timer timer;

  Mat croppedImage = Mat(optImage.blurred);

  timer.start();
  Mat tmp = croppedImage(
      Range(optImage.focusRow, optImage.focusRow + optImage.focused.rows),
      Range(optImage.focusCol, optImage.focusCol + optImage.focused.cols));
  optImage.focused.copyTo(tmp);
  timer.stop("Reconstructing");

  // Reconstruct middle image
  Mat fullLeft, fullCenter, fullRight;

  timer.start();
  Mat fullImage = uncropWrapped(croppedImage, optImage.fullSize.width, optImage.leftBuffer);
  timer.stop("Full image");

  ENSURES(fullImage.size() == optImage.fullSize);

  return fullImage;
}

cv::Mat Optimizer::processImage(const cv::Mat& input,
        int angle, int vAngle) {
  OptimizedImage opt = optimizeImage(input, angle, vAngle);
  return extractImage(opt);
}
