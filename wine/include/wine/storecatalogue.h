/*
 * The Store's display catalogue (displaycatalog.mp.microsoft.com), as parsed by wine/json.h
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/*
 * A product's packages are listed per SKU: Product.DisplaySkuAvailabilities[].Sku.Properties.Packages[], each with
 * its own PackageFullName, PackageFamilyName, PackageFormat (Appx, EAppx, Msix, EMsixBundle, XVC, ...), ContentId
 * and PlatformDependencies; the SKU's FulfillmentData.WuCategoryId is the delivery service's category. A product
 * comes as {"Product":{...}} from /products/ID and as {"Products":[{...}]} from /products?bigIds= and
 * /products/lookup. Used by winegameplatform (the Xbox app's installs) and wineuwp-install (wine-uwp install-store).
 */

#ifndef __WINE_WINE_STORECATALOGUE_H
#define __WINE_WINE_STORECATALOGUE_H

#include "wine/json.h"

struct catalogue_package
{
    const struct json *json;    /* the package object */
    unsigned int sku;           /* its SKU's index */
    unsigned int sku_packages;  /* how many packages that SKU lists */
    int desktop;                /* it names Windows.Desktop among its platforms */
};

/* the product object of either answer, or json_none */
static inline const struct json *catalogue_product( const struct json *root )
{
    const struct json *product = json_get( root, "Product" );
    return product->type == JSON_OBJECT ? product : json_at( json_get( root, "Products" ), 0 );
}

static inline const struct json *catalogue_sku_properties( const struct json *product, unsigned int sku )
{
    return json_get( json_get( json_at( json_get( product, "DisplaySkuAvailabilities" ), sku ), "Sku" ), "Properties" );
}

/* the first WuCategoryId among SKUs 0 to last_sku, or "" */
static inline const char *catalogue_wu_category( const struct json *product, unsigned int last_sku )
{
    unsigned int i, count = json_count( json_get( product, "DisplaySkuAvailabilities" ) );
    const char *category;

    for (i = 0; i < count && i <= last_sku; i++)
    {
        category = json_str( json_get( json_get( catalogue_sku_properties( product, i ), "FulfillmentData" ), "WuCategoryId" ) );
        if (category[0]) return category;
    }
    return "";
}

/* Every package of every SKU, in the catalogue's order; free the array with free(). NULL (and *count 0) if there is
 * none or no memory. */
static inline struct catalogue_package *catalogue_packages( const struct json *product, unsigned int *count )
{
    unsigned int skus = json_count( json_get( product, "DisplaySkuAvailabilities" ) ), total = 0, n = 0, i, k, j;
    const struct json *pkgs, *deps;
    struct catalogue_package *list;

    *count = 0;
    for (i = 0; i < skus; i++) total += json_count( json_get( catalogue_sku_properties( product, i ), "Packages" ) );
    if (!total || !(list = calloc( total, sizeof(*list) ))) return NULL;
    for (i = 0; i < skus; i++)
    {
        pkgs = json_get( catalogue_sku_properties( product, i ), "Packages" );
        for (k = 0; k < json_count( pkgs ); k++)
        {
            list[n].json = json_at( pkgs, k );
            list[n].sku = i;
            list[n].sku_packages = json_count( pkgs );
            deps = json_get( list[n].json, "PlatformDependencies" );
            for (j = 0; j < json_count( deps ); j++)
                list[n].desktop |= !strcmp( json_str( json_get( json_at( deps, j ), "PlatformName" ) ), "Windows.Desktop" );
            n++;
        }
    }
    *count = n;
    return list;
}

/* An EAppx/EAppxBundle/EMsix/EMsixBundle package is DRM-encrypted: its keys come with the licence. */
static inline int catalogue_format_encrypted( const char *format )
{
    return !strncmp( format, "EAppx", 5 ) || !strncmp( format, "EMsix", 5 );
}

/* An Xbox (GDK) package, .xvc/.msixvc: "Xvc", "XVC", "MsixVc"... */
static inline int catalogue_format_xvc( const char *format )
{
    for (; *format; format++)
        if ((format[0] | 0x20) == 'x' && (format[1] | 0x20) == 'v' && (format[2] | 0x20) == 'c') return 1;
    return 0;
}

#endif /* __WINE_WINE_STORECATALOGUE_H */
