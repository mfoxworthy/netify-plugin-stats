include $(TOPDIR)/rules.mk

PKG_NAME:=netify-plugin-stats
PKG_VERSION:=0.1.1
PKG_RELEASE:=1

PKG_SOURCE:=v$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/mfoxworthy/netify-plugin-stats/archive/refs/tags
PKG_SOURCE_SUBDIR:=$(PKG_NAME)-$(PKG_VERSION)
PKG_HASH:=b6a15fed3ac0433faf8f2b12c387fe04dbfc9336a9313a7c6359396bf27a084c

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_FIXUP:=autoreconf
PKG_INSTALL:=1
PKG_BUILD_PARALLEL:=1

PKG_LICENSE:=GPL-3.0-or-later
PKG_MAINTAINER:=mike.foxworthy@gmail.com

include $(INCLUDE_DIR)/package.mk

define Package/netify-plugin-stats
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Netify per-application statistics plugin
  DEPENDS:=+netifyd +libuci +libstdcpp
endef

define Package/netify-plugin-stats/description
  Netify Agent processor plugin that accumulates per-application and
  per-category traffic statistics into a ring-buffer store and exposes
  them via an rpcd/ubus interface for the LuCI statistics UI.
endef

define Package/netify-plugin-stats/conffiles
/etc/config/netify-stats
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	mkdir -p $(PKG_BUILD_DIR)/m4 $(PKG_BUILD_DIR)/automake
endef

CONFIGURE_ARGS += --enable-package --disable-tests

define Package/netify-plugin-stats/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/lib/netify-plugin-stats.so $(1)/usr/lib/

	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/netify-stats-query $(1)/usr/bin/

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(CURDIR)/files/etc/config/netify-stats $(1)/etc/config/

	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) $(CURDIR)/files/usr/libexec/rpcd/luci.netify-stats $(1)/usr/libexec/rpcd/

	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) $(CURDIR)/files/etc/uci-defaults/90-netify-plugin-stats $(1)/etc/uci-defaults/
endef

$(eval $(call BuildPackage,netify-plugin-stats))
