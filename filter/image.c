/*
 * "$Id: image.c,v 1.5 1998/07/28 20:48:30 mike Exp $"
 *
 *   Base image support for espPrint, a collection of printer drivers.
 *
 *   Copyright 1993-1998 by Easy Software Products
 *
 *   These coded instructions, statements, and computer programs contain
 *   unpublished proprietary information of Easy Software Products, and
 *   are protected by Federal copyright law.  They may not be disclosed
 *   to third parties or copied or duplicated in any form, in whole or
 *   in part, without the prior written consent of Easy Software Products.
 *
 * Contents:
 *
 * Revision History:
 *
 *   $Log: image.c,v $
 *   Revision 1.5  1998/07/28 20:48:30  mike
 *   Changed IMAGE_MAX_CACHE to RIP_MAX_CACHE.
 *   Now compute minimum required tiles to avoid cache thrashing.
 *   Fixed bug in flush_tile() - wasn't clearing cache pointer if tile wasn't
 *   dirty.
 *
 *   Revision 1.4  1998/07/07 13:04:52  mike
 *   OK, updated ImageSetMaxTiles() again, but this time we look at the
 *   IMAGE_MAX_CACHE environment variable and compute things that way.
 *
 *   Revision 1.3  1998/07/07 12:56:23  mike
 *   Updated ImageSetMaxTiles() to ignore the image depth so that images < 64MB
 *   won't be cached...
 *
 *   Revision 1.2  1998/03/19  16:15:15  mike
 *   Changed rewind() to fseek() to fix bug in HP-UX version of image library.
 *
 *   Revision 1.1  1998/02/19  20:43:33  mike
 *   Initial revision
 */

/*
 * Include necessary headers...
 */

/*#define DEBUG*/
#include "image.h"
#include <unistd.h>
#include <ctype.h>


/*
 * Local functions...
 */


static ib_t	*get_tile(image_t *img, int x, int y);
static void	flush_tile(image_t *img);


image_t *
ImageOpen(char *filename,
          int  primary,
          int  secondary,
          int  saturation,
          int  hue)
{
  FILE		*fp;		/* File pointer */
  unsigned char	header[16];	/* First 16 bytes of file */
  image_t	*img;		/* New image buffer */
  int		status;		/* Status of load... */


 /*
  * Range check...
  */

  if (filename == NULL)
    return (NULL);

 /*
  * Figure out the file type...
  */

  if ((fp = fopen(filename, "r")) == NULL)
    return (NULL);

  if (fread(header, 1, sizeof(header), fp) == 0)
  {
    fclose(fp);
    return (NULL);
  };

  fseek(fp, 0, SEEK_SET);

 /*
  * Allocate memory...
  */

  img = calloc(sizeof(image_t), 1);

  if (img == NULL)
  {
    fclose(fp);
    return (NULL);
  };

 /*
  * Load the image as appropriate...
  */

  img->max_ics = TILE_MINIMUM;
  img->xppi    = 128;
  img->yppi    = 128;

  if (memcmp(header, "\211PNG", 4) == 0)
    status = ImageReadPNG(img, fp, primary, secondary, saturation, hue);
  else if (memcmp(header + 6, "JFIF", 4) == 0)
    status = ImageReadJPEG(img, fp, primary, secondary, saturation, hue);
  else if (memcmp(header, "GIF87a", 6) == 0 ||
           memcmp(header, "GIF89a", 6) == 0)
    status = ImageReadGIF(img, fp, primary, secondary, saturation, hue);
  else if (header[0] == 0x01 && header[1] == 0xda)
    status = ImageReadSGI(img, fp, primary, secondary, saturation, hue);
  else if (header[0] == 0x59 && header[1] == 0xa6 &&
           header[2] == 0x6a && header[3] == 0x95)
    status = ImageReadSunRaster(img, fp, primary, secondary, saturation, hue);
  else if (memcmp(header, "MM", 2) == 0 ||
           memcmp(header, "II", 2) == 0)
    status = ImageReadTIFF(img, fp, primary, secondary, saturation, hue);
  else if (memcmp(header, "\377\377\377\377", 4) == 0)
    status = ImageReadPhotoCD(img, fp, primary, secondary, saturation, hue);
  else if (header[0] == 'P' && header[1] >= '1' && header[1] <= '6')
    status = ImageReadPNM(img, fp, primary, secondary, saturation, hue);
  else
  {
    fclose(fp);
    status = -1;
  };

  if (status)
  {
    free(img);
    return (NULL);
  }
  else
    return (img);
}


void
ImageClose(image_t *img)
{
  ic_t	*current,
	*next;


 /*
  * Free the image cache...
  */

  for (current = img->first; current != NULL; current = next)
  {
    next = current->next;
    free(current);
  };

 /*
  * Wipe the tile cache file (if any)...
  */

  if (img->cachefile != NULL)
  {
    fclose(img->cachefile);
    unlink(img->cachename);
  };

 /*
  * Free the rest of memory...
  */

  if (img->tiles != NULL)
  {
    free(img->tiles[0]);
    free(img->tiles);
  };

  free(img);
}


/*
 * 'ImageSetMaxTiles()' - Set the maximum number of tiles to cache.
 *
 * If the "max_tiles" argument is 0 then the maximum number of tiles is
 * computed from the image size.
 */

void
ImageSetMaxTiles(image_t *img,		/* I - Image to set */
                 int     max_tiles)	/* I - Number of tiles to cache */
{
  int	cache_size,			/* Size of tile cache in bytes */
	min_tiles,			/* Minimum number of tiles to cache */
	max_size;			/* Maximum cache size in bytes */
  char	*cache_env,			/* Cache size environment variable */
	cache_units[255];		/* Cache size units */


  min_tiles = max(TILE_MINIMUM,
                  1 + max((img->xsize + TILE_SIZE - 1) / TILE_SIZE,
                          (img->ysize + TILE_SIZE - 1) / TILE_SIZE));

  if (max_tiles == 0)
    max_tiles = ((img->xsize + TILE_SIZE - 1) / TILE_SIZE) *
                ((img->ysize + TILE_SIZE - 1) / TILE_SIZE);

  cache_size = max_tiles * TILE_SIZE * TILE_SIZE * ImageGetDepth(img);

  if ((cache_env = getenv("RIP_MAX_CACHE")) != NULL)
  {
    switch (sscanf(cache_env, "%d%s", &max_size, cache_units))
    {
      case 0 :
          max_size = 32 * 1024 * 1024;
	  break;
      case 1 :
          max_size *= 4 * TILE_SIZE * TILE_SIZE;
	  break;
      case 2 :
          if (tolower(cache_units[0]) == 'g')
	    max_size *= 1024 * 1024 * 1024;
          else if (tolower(cache_units[0]) == 'm')
	    max_size *= 1024 * 1024;
	  else if (tolower(cache_units[0]) == 'k')
	    max_size *= 1024;
	  else if (tolower(cache_units[0]) == 't')
	    max_size *= 4 * TILE_SIZE * TILE_SIZE;
	  break;
    };
  }
  else
    max_size = 32 * 1024 * 1024;

  if (cache_size > max_size)
    max_tiles = max_size / TILE_SIZE / TILE_SIZE / ImageGetDepth(img);

  if (max_tiles < min_tiles)
    max_tiles = min_tiles;

  img->max_ics = max_tiles;

#ifdef DEBUG
  fprintf(stderr, "ImageSetMaxTiles: max_ics=%d...\n", img->max_ics);
#endif /* DEBUG */
}


/*
 * 'ImageGetCol()' - Get a column of pixels from an image.
 */

int
ImageGetCol(image_t *img,
            int     x,
            int     y,
            int     height,
            ib_t    *pixels)
{
  int	bpp,
	twidth,
	count;
  ib_t	*ib;


  if (img == NULL || x < 0 || x >= img->xsize || y >= img->ysize)
    return (-1);

  if (y < 0)
  {
    height += y;
    y = 0;
  };

  if ((y + height) > img->ysize)
    height = img->ysize - y;

  if (height < 1)
    return (-1);

  bpp    = ImageGetDepth(img);
  twidth = bpp * (TILE_SIZE - 1);

  while (height > 0)
  {
    ib = get_tile(img, x, y);

    if (ib == NULL)
      return (-1);

    count = TILE_SIZE - (y & (TILE_SIZE - 1));
    if (count > height)
      count = height;

    y      += count;
    height -= count;

    for (; count > 0; count --, ib += twidth)
      switch (bpp)
      {
        case 4 :
            *pixels++ = *ib++;
        case 3 :
            *pixels++ = *ib++;
            *pixels++ = *ib++;
        case 1 :
            *pixels++ = *ib++;
            break;
      };
  };

  return (0);
}


int
ImageGetRow(image_t *img,
            int     x,
            int     y,
            int     width,
            ib_t    *pixels)
{
  int	bpp,
	count;
  ib_t	*ib;


  if (img == NULL || y < 0 || y >= img->ysize || x >= img->xsize)
    return (-1);

  if (x < 0)
  {
    width += x;
    x = 0;
  };

  if ((x + width) > img->xsize)
    width = img->xsize - x;

  if (width < 1)
    return (-1);

  bpp = img->colorspace < 0 ? -img->colorspace : img->colorspace;

  while (width > 0)
  {
    ib = get_tile(img, x, y);

    if (ib == NULL)
      return (-1);

    count = TILE_SIZE - (x & (TILE_SIZE - 1));
    if (count > width)
      count = width;
    memcpy(pixels, ib, count * bpp);
    pixels += count * bpp;
    x      += count;
    width  -= count;
  };

  return (0);
}


int
ImagePutCol(image_t *img,
            int     x,
            int     y,
            int     height,
            ib_t    *pixels)
{
  int	bpp,
	twidth,
	count;
  int	tilex,
	tiley;
  ib_t	*ib;


  if (img == NULL || x < 0 || x >= img->xsize || y >= img->ysize)
    return (-1);

  if (y < 0)
  {
    height += y;
    y = 0;
  };

  if ((y + height) > img->ysize)
    height = img->ysize - y;

  if (height < 1)
    return (-1);

  bpp    = ImageGetDepth(img);
  twidth = bpp * (TILE_SIZE - 1);
  tilex  = x / TILE_SIZE;
  tiley  = y / TILE_SIZE;

  while (height > 0)
  {
    ib = get_tile(img, x, y);

    if (ib == NULL)
      return (-1);

    img->tiles[tiley][tilex].dirty = 1;
    tiley ++;

    count = TILE_SIZE - (y & (TILE_SIZE - 1));
    if (count > height)
      count = height;

    y      += count;
    height -= count;

    for (; count > 0; count --, ib += twidth)
      switch (bpp)
      {
        case 4 :
            *ib++ = *pixels++;
        case 3 :
            *ib++ = *pixels++;
            *ib++ = *pixels++;
        case 1 :
            *ib++ = *pixels++;
            break;
      };
  };

  return (0);
}


int
ImagePutRow(image_t *img,
            int     x,
            int     y,
            int     width,
            ib_t    *pixels)
{
  int	bpp,
	count;
  int	tilex,
	tiley;
  ib_t	*ib;


  if (img == NULL || y < 0 || y >= img->ysize || x >= img->xsize)
    return (-1);

  if (x < 0)
  {
    width += x;
    x = 0;
  };

  if ((x + width) > img->xsize)
    width = img->xsize - x;

  if (width < 1)
    return (-1);

  bpp   = img->colorspace < 0 ? -img->colorspace : img->colorspace;
  tilex = x / TILE_SIZE;
  tiley = y / TILE_SIZE;

  while (width > 0)
  {
    ib = get_tile(img, x, y);

    if (ib == NULL)
      return (-1);

    img->tiles[tiley][tilex].dirty = 1;

    count = TILE_SIZE - (x & (TILE_SIZE - 1));
    if (count > width)
      count = width;
    memcpy(ib, pixels, count * bpp);
    pixels += count * bpp;
    x      += count;
    width  -= count;
    tilex  ++;
  };

  return (0);
}


static ib_t *
get_tile(image_t *img,
         int     x,
         int     y)
{
  int		bpp,
		tilex,
		tiley,
		xtiles,
		ytiles;
  ic_t		*ic;
  itile_t	*tile;


  if (img->tiles == NULL)
  {
    xtiles = (img->xsize + TILE_SIZE - 1) / TILE_SIZE;
    ytiles = (img->ysize + TILE_SIZE - 1) / TILE_SIZE;

#ifdef DEBUG
    fprintf(stderr, "get_tile: Creating tile array (%dx%d)\n", xtiles, ytiles);
#endif /* DEBUG */

    img->tiles = calloc(sizeof(itile_t *), ytiles);
    tile       = calloc(sizeof(itile_t), xtiles * ytiles);

    for (tiley = 0; tiley < ytiles; tiley ++)
    {
      img->tiles[tiley] = tile;
      for (tilex = xtiles; tilex > 0; tilex --, tile ++)
        tile->pos = -1;
    };
  };

  bpp   = ImageGetDepth(img);
  tilex = x / TILE_SIZE;
  tiley = y / TILE_SIZE;
  x     &= (TILE_SIZE - 1);
  y     &= (TILE_SIZE - 1);

  tile = img->tiles[tiley] + tilex;

  if ((ic = tile->ic) == NULL)
  {
    if (img->num_ics < img->max_ics)
    {
#ifdef DEBUG
      fputs("get_tile: Allocating new cache tile...\n", stderr);
#endif /* DEBUG */

      ic         = calloc(sizeof(ic_t) + bpp * TILE_SIZE * TILE_SIZE, 1);
      ic->pixels = ((ib_t *)ic) + sizeof(ic_t);

      img->num_ics ++;
    }
    else
    {
#ifdef DEBUG
      fputs("get_tile: Flushing old cache tile...\n", stderr);
#endif /* DEBUG */

      flush_tile(img);
      ic = img->first;
    };

    ic->tile = tile;
    tile->ic = ic;

    if (tile->pos >= 0)
    {
#ifdef DEBUG
      fprintf(stderr, "get_tile: loading cache tile from file position %d...\n",
              tile->pos);
#endif /* DEBUG */

      if (ftell(img->cachefile) != tile->pos)
        if (fseek(img->cachefile, tile->pos, SEEK_SET))
	  perror("get_tile:");

      fread(ic->pixels, bpp, TILE_SIZE * TILE_SIZE, img->cachefile);
    }
    else
    {
#ifdef DEBUG
      fputs("get_tile: Clearing cache tile...\n", stderr);
#endif /* DEBUG */

      memset(ic->pixels, 0, bpp * TILE_SIZE * TILE_SIZE);
    };
  };

  if (ic == img->first)
    img->first = ic->next;
  else if (img->first == NULL)
    img->first = ic;

  if (ic != img->last)
  {
    if (img->last != NULL)
      img->last->next = ic;

    ic->prev  = img->last;
    img->last = ic;
  };

  ic->next = NULL;

  return (ic->pixels + bpp * (y * TILE_SIZE + x));
}


static void
flush_tile(image_t *img)
{
  int		bpp;
  itile_t	*tile;



#ifdef DEBUG
  fprintf(stderr, "flush_tile(%08x)...\n", img);
#endif /* DEBUG */

  bpp  = ImageGetDepth(img);
  tile = img->first->tile;

  if (!tile->dirty)
  {
    tile->ic = NULL;
    return;
  };

  if (img->cachefile == NULL)
  {
    tmpnam(img->cachename);

#ifdef DEBUG
    fprintf(stderr, "flush_tile: Creating cache file %s...\n", img->cachename);
#endif /* DEBUG */

    if ((img->cachefile = fopen(img->cachename, "wb+")) == NULL)
    {
      fprintf(stderr, "flush_tile: Unable to create swap file - %s\n",
              strerror(errno));
      return;
    };
  };

  if (tile->pos >= 0)
  {
    if (ftell(img->cachefile) != tile->pos)
      if (fseek(img->cachefile, tile->pos, SEEK_SET))
        perror("flush_tile:");
  }
  else
  {
    if (fseek(img->cachefile, 0, SEEK_END))
      perror("flush_tile:");

    tile->pos = ftell(img->cachefile);
  };

#ifdef DEBUG
  fprintf(stderr, "flush_tile: Wrote tile cache at position %d...\n",
          tile->pos);
#endif /* DEBUG */

  fwrite(tile->ic->pixels, bpp, TILE_SIZE * TILE_SIZE, img->cachefile);
  tile->ic    = NULL;
  tile->dirty = 0;
}


/*
 * End of "$Id: image.c,v 1.5 1998/07/28 20:48:30 mike Exp $".
 */
