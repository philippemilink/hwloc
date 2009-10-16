/*
 * Copyright © 2009 CNRS, INRIA, Université Bordeaux 1
 * See COPYING in top-level directory.
 */

#include <private/config.h>
#include <hwloc.h>
#include <hwloc/helper.h>
#include <private/private.h>
#include <private/debug.h>

#ifdef HAVE_LIBPCI

#include <pci/pci.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#ifdef LINUX_SYS
#include <hwloc/linux.h>
#endif

#define CONFIG_SPACE_CACHESIZE 64

static void
hwloc_pci_traverse(struct hwloc_obj *root, int depth)
{
  struct hwloc_obj *child = root->first_child;
  while (child) {
    char busid[14];
    snprintf(busid, sizeof(busid), "%04x:%02x:%02x.%01x",
             child->attr->pcidev.domain, child->attr->pcidev.bus, child->attr->pcidev.dev, child->attr->pcidev.func);

    if (child->type == HWLOC_OBJ_BRIDGE) {
      if (child->attr->bridge.upstream_type == HWLOC_OBJ_BRIDGE_HOST)
	hwloc_debug("%*s HostBridge", depth, "");
      else
	hwloc_debug("%*s %s Bridge [%04x:%04x]", depth, "", busid,
		    child->attr->pcidev.vendor_id, child->attr->pcidev.device_id);
      hwloc_debug(" to %04x:[%02x:%02x]\n",
		  child->attr->bridge.downstream.pci.domain, child->attr->bridge.downstream.pci.secondary_bus, child->attr->bridge.downstream.pci.subordinate_bus);
    } else
      hwloc_debug("%*s %s Device [%04x:%04x (%04x:%04x) rev=%02x class=%04x]\n", depth, "", busid,
		  child->attr->pcidev.vendor_id, child->attr->pcidev.device_id,
		  child->attr->pcidev.subvendor_id, child->attr->pcidev.subdevice_id,
		  child->attr->pcidev.revision, child->attr->pcidev.class);

    hwloc_pci_traverse(child, depth+2);
    child = child->next_sibling;
  }
}

static void
hwloc_pci_set_bridge_depths(struct hwloc_obj *root, int depth)
{
  struct hwloc_obj *child = root->first_child;
  while (child) {
    if (child->type == HWLOC_OBJ_BRIDGE) {
      child->attr->bridge.depth = depth;
      hwloc_pci_set_bridge_depths(child, depth+1);
    }
    child = child->next_sibling;
  }
}

enum hwloc_pci_busid_comparison_e {
  HWLOC_PCI_BUSID_LOWER,
  HWLOC_PCI_BUSID_HIGHER,
  HWLOC_PCI_BUSID_INCLUDED,
  HWLOC_PCI_BUSID_SUPERSET,
};

static int
hwloc_pci_compare_busids(struct hwloc_obj *a, struct hwloc_obj *b)
{
  if (a->type == HWLOC_OBJ_BRIDGE)
    assert(a->attr->bridge.upstream_type == HWLOC_OBJ_BRIDGE_PCI);
  if (b->type == HWLOC_OBJ_BRIDGE)
    assert(b->attr->bridge.upstream_type == HWLOC_OBJ_BRIDGE_PCI);

  if (a->attr->pcidev.domain < b->attr->pcidev.domain)
    return HWLOC_PCI_BUSID_LOWER;
  if (a->attr->pcidev.domain > b->attr->pcidev.domain)
    return HWLOC_PCI_BUSID_HIGHER;

  if (a->type == HWLOC_OBJ_BRIDGE
      && b->attr->pcidev.bus >= a->attr->bridge.downstream.pci.secondary_bus
      && b->attr->pcidev.bus <= a->attr->bridge.downstream.pci.subordinate_bus)
    return HWLOC_PCI_BUSID_SUPERSET;
  if (b->type == HWLOC_OBJ_BRIDGE
      && a->attr->pcidev.bus >= b->attr->bridge.downstream.pci.secondary_bus
      && a->attr->pcidev.bus <= b->attr->bridge.downstream.pci.subordinate_bus)
    return HWLOC_PCI_BUSID_INCLUDED;

  if (a->attr->pcidev.bus < b->attr->pcidev.bus)
    return HWLOC_PCI_BUSID_LOWER;
  if (a->attr->pcidev.bus > b->attr->pcidev.bus)
    return HWLOC_PCI_BUSID_HIGHER;

  if (a->attr->pcidev.dev < b->attr->pcidev.dev)
    return HWLOC_PCI_BUSID_LOWER;
  if (a->attr->pcidev.dev > b->attr->pcidev.dev)
    return HWLOC_PCI_BUSID_HIGHER;

  if (a->attr->pcidev.func < b->attr->pcidev.func)
    return HWLOC_PCI_BUSID_LOWER;
  if (a->attr->pcidev.func > b->attr->pcidev.func)
    return HWLOC_PCI_BUSID_HIGHER;

  assert(0);
}

static void
hwloc_pci_add_child_before(struct hwloc_obj *root, struct hwloc_obj *child, struct hwloc_obj *new)
{
  if (child) {
    new->prev_sibling = child->prev_sibling;
    child->prev_sibling = new;
  } else {
    new->prev_sibling = root->last_child;
    root->last_child = new;
  }

  if (new->prev_sibling)
    new->prev_sibling->next_sibling = new;
  else
    root->first_child = new;
  new->next_sibling = child;
}

static void
hwloc_pci_remove_child(struct hwloc_obj *root, struct hwloc_obj *child)
{
  if (child->next_sibling)
    child->next_sibling->prev_sibling = child->prev_sibling;
  else
    root->last_child = child->prev_sibling;
  if (child->prev_sibling)
    child->prev_sibling->next_sibling = child->next_sibling;
  else
    root->first_child = child->next_sibling;
  child->prev_sibling = NULL;
  child->next_sibling = NULL;
}

static void hwloc_pci_add_object(struct hwloc_obj *root, struct hwloc_obj *new);

static void
hwloc_pci_try_insert_siblings_below_new_bridge(struct hwloc_obj *root, struct hwloc_obj *new)
{
  struct hwloc_obj *current, *next;

  next = new->next_sibling;
  while (next) {
    current = next;
    next = current->next_sibling;

    enum hwloc_pci_busid_comparison_e comp = hwloc_pci_compare_busids(current, new);
    assert(comp != HWLOC_PCI_BUSID_SUPERSET);
    if (comp == HWLOC_PCI_BUSID_HIGHER)
      continue;
    assert(comp == HWLOC_PCI_BUSID_INCLUDED);

    /* move this object below the new bridge */
    hwloc_pci_remove_child(root, current);
    hwloc_pci_add_object(new, current);
  }
}

static void
hwloc_pci_add_object(struct hwloc_obj *root, struct hwloc_obj *new)
{
  struct hwloc_obj *current;

  current = root->first_child;
  while (current) {
    enum hwloc_pci_busid_comparison_e comp = hwloc_pci_compare_busids(new, current);
    switch (comp) {
    case HWLOC_PCI_BUSID_HIGHER:
      /* go further */
      current = current->next_sibling;
      continue;
    case HWLOC_PCI_BUSID_INCLUDED:
      /* insert below current bridge */
      hwloc_pci_add_object(current, new);
      return;
    case HWLOC_PCI_BUSID_LOWER:
    case HWLOC_PCI_BUSID_SUPERSET:
      /* insert before current object */
      hwloc_pci_add_child_before(root, current, new);
      /* walk next siblings and move them below new bridge if needed */
      hwloc_pci_try_insert_siblings_below_new_bridge(root, new);
      return;
    }
  }
  /* add to the end of the list if higher than everybody */
  hwloc_pci_add_child_before(root, NULL, new);
}

static void
hwloc_pci_drop_useless_bridges(struct hwloc_obj *root)
{
  struct hwloc_obj *child, *next_child;
  next_child = root->first_child;
  while (next_child) {
    child = next_child;
    next_child = child->next_sibling;
    if (child->type != HWLOC_OBJ_BRIDGE)
      continue;
    hwloc_pci_drop_useless_bridges(child);
    if (!child->first_child) {
      hwloc_pci_remove_child(root, child);
      free_object(child);
    }
  }
}

static struct hwloc_obj *
hwloc_pci_find_hostbridge_parent(struct hwloc_topology *topology, struct hwloc_obj *hostbridge)
{
  hwloc_cpuset_t cpuset;

  /* override the cpuset with the environment if given */
  char envname[256];
  snprintf(envname, sizeof(envname), "HWLOC_PCI_%04x_%02x_LOCALCPUS",
	   hostbridge->first_child->attr->pcidev.domain, hostbridge->first_child->attr->pcidev.bus);
  char *env = getenv(envname);
  if (env) {
    hwloc_debug("Overriding localcpus using %s in the environment\n", envname);
    cpuset = hwloc_cpuset_from_string(env);
    goto found;
  }

  /* get the hostbridge cpuset. it's not a PCI device, so we use its first child locality info */
#ifdef LINUX_SYS
  char path[256];
  FILE *file;
  snprintf(path, sizeof(path), "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/local_cpus",
	   hostbridge->first_child->attr->pcidev.domain, hostbridge->first_child->attr->pcidev.bus,
	   hostbridge->first_child->attr->pcidev.dev, hostbridge->first_child->attr->pcidev.func);
  file = fopen(path, "r"); /* the libpci backend doesn't use sysfs.fsroot */
  cpuset = hwloc_linux_parse_cpumap_file(file);
  fclose(file);
  if (cpuset)
    goto found;
#endif

  /* if we got nothing, assume the hostbridge is attached to the top of hierarchy */
  cpuset = hwloc_cpuset_alloc();
  hwloc_cpuset_fill(cpuset);

 found:
  hwloc_debug_cpuset("Attaching hostbridge to cpuset %s\n", cpuset);

  /* attach the hostbridge now that it contains the right objects */
  struct hwloc_obj *parent = hwloc_get_obj_covering_cpuset(topology, cpuset);
  /* if found nothing, attach to top */
  if (!parent)
    parent = topology->levels[0][0];
  /* do not attach to the lowest object since it could be a cache or so,
   * go up as long as the cpuset is the same
   */
  while (parent->father && hwloc_cpuset_isequal(parent->cpuset, parent->father->cpuset))
    parent = parent->father;

  hwloc_cpuset_free(cpuset);

  return parent;
}

void
hwloc_look_libpci(struct hwloc_topology *topology)
{
  struct pci_access *pciaccess;
  struct pci_dev *pcidev;
  struct hwloc_obj fakehostbridge; /* temporary object covering the whole PCI hierarchy until its complete */

  fakehostbridge.first_child = NULL;
  fakehostbridge.last_child = NULL;

  hwloc_debug("\nScanning PCI buses...\n");

  pciaccess = pci_alloc();
  pci_init(pciaccess);
  pci_scan_bus(pciaccess);

  pcidev = pciaccess->devices;
  while (pcidev) {
    char name[128];
    u8 config_space_cache[CONFIG_SPACE_CACHESIZE];
    struct hwloc_obj *obj;
    unsigned char headertype;
    unsigned os_index;
    unsigned baseclass;
    unsigned isbridge;

    /* cache what we need of the config space */
    pci_read_block(pcidev, 0, config_space_cache, CONFIG_SPACE_CACHESIZE);

    /* is this a bridge? */
    HWLOC_BUILD_ASSERT(PCI_HEADER_TYPE < CONFIG_SPACE_CACHESIZE);
    headertype = config_space_cache[PCI_HEADER_TYPE] & 0x7f;
    isbridge = (pcidev->device_class == PCI_CLASS_BRIDGE_PCI
		&& headertype == PCI_HEADER_TYPE_BRIDGE);

    /* is this device interesting? */
    baseclass = pcidev->device_class >> 8;
    if (!(topology->flags & HWLOC_TOPOLOGY_FLAG_WHOLE_PCI)
	&& !isbridge
	&& baseclass != PCI_BASE_CLASS_DISPLAY
	&& baseclass != PCI_BASE_CLASS_NETWORK
	&& baseclass != PCI_BASE_CLASS_STORAGE
	&& pcidev->device_class != PCI_CLASS_SERIAL_INFINIBAND)
      goto nextdev;

    /* might be useful for debugging (note that domain might be truncated) */
    os_index = (pcidev->domain << 20) + (pcidev->bus << 12) + (pcidev->dev << 4) + pcidev->func;

    obj = hwloc_alloc_setup_object(isbridge ? HWLOC_OBJ_BRIDGE : HWLOC_OBJ_PCI_DEVICE, os_index);
    obj->cpuset = hwloc_cpuset_alloc();
    obj->attr->pcidev.domain = pcidev->domain;
    obj->attr->pcidev.bus = pcidev->bus;
    obj->attr->pcidev.dev = pcidev->dev;
    obj->attr->pcidev.func = pcidev->func;
    obj->attr->pcidev.vendor_id = pcidev->vendor_id;
    obj->attr->pcidev.device_id = pcidev->device_id;
    obj->attr->pcidev.class = pcidev->device_class;
    HWLOC_BUILD_ASSERT(PCI_REVISION_ID < CONFIG_SPACE_CACHESIZE);
    obj->attr->pcidev.revision = config_space_cache[PCI_REVISION_ID];
    HWLOC_BUILD_ASSERT(PCI_SUBSYSTEM_VENDOR_ID < CONFIG_SPACE_CACHESIZE);
    obj->attr->pcidev.subvendor_id = config_space_cache[PCI_SUBSYSTEM_VENDOR_ID];
    HWLOC_BUILD_ASSERT(PCI_SUBSYSTEM_ID < CONFIG_SPACE_CACHESIZE);
    obj->attr->pcidev.subdevice_id = config_space_cache[PCI_SUBSYSTEM_ID];

    if (isbridge) {
      HWLOC_BUILD_ASSERT(PCI_PRIMARY_BUS < CONFIG_SPACE_CACHESIZE);
      HWLOC_BUILD_ASSERT(PCI_SECONDARY_BUS < CONFIG_SPACE_CACHESIZE);
      HWLOC_BUILD_ASSERT(PCI_SUBORDINATE_BUS < CONFIG_SPACE_CACHESIZE);
      assert(config_space_cache[PCI_PRIMARY_BUS] == pcidev->bus);
      obj->attr->bridge.upstream_type = HWLOC_OBJ_BRIDGE_PCI;
      obj->attr->bridge.downstream_type = HWLOC_OBJ_BRIDGE_PCI;
      obj->attr->bridge.downstream.pci.domain = pcidev->domain;
      obj->attr->bridge.downstream.pci.secondary_bus = config_space_cache[PCI_SECONDARY_BUS];
      obj->attr->bridge.downstream.pci.subordinate_bus = config_space_cache[PCI_SUBORDINATE_BUS];
    }

    strcpy(name, "??");
    if (pci_lookup_name(pciaccess, name, sizeof(name),
			PCI_LOOKUP_VENDOR|PCI_LOOKUP_DEVICE|PCI_LOOKUP_NO_NUMBERS,
			pcidev->vendor_id, pcidev->device_id))
      obj->name = strdup(name);
    hwloc_debug("  %04x:%02x:%02x.%01x %04x %04x:%04x %s\n",
		pcidev->domain, pcidev->bus, pcidev->dev, pcidev->func,
		pcidev->device_class, pcidev->vendor_id, pcidev->device_id,
		name);

    hwloc_pci_add_object(&fakehostbridge, obj);

  nextdev:
    pcidev = pcidev->next;
  }

  pci_cleanup(pciaccess);

  hwloc_debug("\nPCI hierarchy after basic scan:\n");
  hwloc_pci_traverse(&fakehostbridge, 0);

  /* drop useless bridges if needed */
  if (!(topology->flags & HWLOC_TOPOLOGY_FLAG_WHOLE_PCI))
    hwloc_pci_drop_useless_bridges(&fakehostbridge);

  hwloc_debug("\nPCI hierarchy removing useless objects:\n");
  hwloc_pci_traverse(&fakehostbridge, 0);

  if (!fakehostbridge.first_child)
    /* found nothing, exit */
    return;

  /* walk the hierarchy and set bridge depth */
  hwloc_pci_set_bridge_depths(&fakehostbridge, 0);

  /*
   * fakehostbridge lists all objects connected to any upstream bus in the machine.
   * We now create one real hostbridge object per upstream bus.
   * It's not actually a PCI device so we have to create it.
   */
  unsigned current_hostbridge = 0;
  while (fakehostbridge.first_child) {
    /* start a new host bridge */
    struct hwloc_obj *hostbridge = hwloc_alloc_setup_object(HWLOC_OBJ_BRIDGE, current_hostbridge++);
    struct hwloc_obj *child = fakehostbridge.first_child;
    struct hwloc_obj *next_child;
    unsigned short current_domain = child->attr->pcidev.domain;
    unsigned char current_bus = child->attr->pcidev.bus;
    unsigned char current_subordinate = current_bus;
    hwloc_debug("Starting new PCI hostbridge %04x:%02x\n", current_domain, current_bus);

    /*
     * attach all objects from the same upstream domain/bus
     */
  next_child:
    next_child = child->next_sibling;
    hwloc_pci_remove_child(&fakehostbridge, child);
    hwloc_pci_add_child_before(hostbridge, NULL, child);

    /* compute hostbrigde secondary/subordinate buses */
    if (child->type == HWLOC_OBJ_BRIDGE
	&& child->attr->bridge.downstream.pci.subordinate_bus > current_subordinate)
      current_subordinate = child->attr->bridge.downstream.pci.subordinate_bus;

    /* use next child if it has the same domains/bus */
    child = next_child;
    if (child
	&& child->attr->pcidev.domain == current_domain
	&& child->attr->pcidev.bus == current_bus)
      goto next_child;

    /* finish setting up this hostbridge */
    hostbridge->cpuset = hwloc_cpuset_alloc();
    hostbridge->attr->bridge.upstream_type = HWLOC_OBJ_BRIDGE_HOST;
    hostbridge->attr->bridge.downstream_type = HWLOC_OBJ_BRIDGE_PCI;
    hostbridge->attr->bridge.downstream.pci.domain = current_domain;
    hostbridge->attr->bridge.downstream.pci.secondary_bus = current_bus;
    hostbridge->attr->bridge.downstream.pci.subordinate_bus = current_subordinate;
    hwloc_debug("New PCI hostbridge %04x:[%02x-%02x]\n",
		current_domain, current_bus, current_subordinate);

    /* attach the hostbridge where it belongs */
    struct hwloc_obj *parent = hwloc_pci_find_hostbridge_parent(topology, hostbridge);
    hwloc_insert_object_by_parent(topology, parent, hostbridge);
  }
}

#endif /* HAVE_LIBPCI */
