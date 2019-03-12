// SPDX-License-Identifier: GPL-2.0
/*
 * boogiepop.c: A library for Peripheral-to-Peripheral communication.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/genalloc.h>
#include <linux/pci-p2pdma.h>

#include <libpop.h>

/* XXX: define struct pci_p2pdma, wichi is defined in the local scope
 * of p2pdma.c, in this scope. to get size of p2pmem. */
struct pci_p2pdma {
        struct percpu_ref devmap_ref;
        struct completion devmap_ref_done;
        struct gen_pool *pool;
        bool p2pmem_published;
};

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define POP_VERSION "0.0.0"


/* structure describing p2pdma-capable devices */
struct pop_dev {
	struct list_head	list;	/* pop.dev_list */

#define DEVNAMELEN	32
	struct pci_dev *pdev;	/* target pci dev with p2pmem		*/
	char	devname[DEVNAMELEN];	/* pop/DOMAIN:BUS:SLOT.FUNC	*/
	void	*p2pmem;	/* p2pmem of the above pci dev		*/
	size_t	size;		/* p2pmem size	*/

	struct miscdevice	mdev;	/* char dev for this pop_dev */
	atomic_t		refcnt;
};

/* structure describing boogiepop kernel module */
struct boogiepop {
	struct list_head	dev_list;	/* list of pop_dev */
};
static struct boogiepop pop;

static struct pop_dev *pop_find_dev(struct pci_dev *pdev)
{
	struct pop_dev *ppdev;

	list_for_each_entry(ppdev, &pop.dev_list, list) {
		if (ppdev->pdev == pdev)
			return ppdev;
	}

	return NULL;
}


static struct pop_dev *pop_find_dev_by_bus_and_slot(int domain,
						    unsigned int bus,
						    unsigned int devfn)
{
	struct pci_dev *pdev;
	struct pop_dev *ppdev;

	pdev = pci_get_domain_bus_and_slot(domain, bus, devfn);
	if (!pdev)
		return NULL;

	ppdev = pop_find_dev(pdev);
	pci_dev_put(pdev);

	return ppdev;
}

static int pop_dev_open(struct inode *inode, struct file *filp)
{
	int domain, ret;
	unsigned int bus, slot, func;
	struct pop_dev *ppdev;

	ret = sscanf(filp->f_path.dentry->d_name.name, "%x:%x:%x.%x",
		     &domain, &bus, &slot, &func);
	if (unlikely(ret < 4)) {
		pr_err("%s: invalid pop dev name %s\n", __func__,
		       filp->f_path.dentry->d_name.name);
		return -EINVAL;
	}

	ppdev = pop_find_dev_by_bus_and_slot(domain, bus,
					     PCI_DEVFN(slot, func));
	if (!ppdev) {
		pr_err("%s: %s is not registered as pop dev\n", __func__,
		       filp->f_path.dentry->d_name.name);
		return -EINVAL;
	}

	atomic_inc(&ppdev->refcnt);
	filp->private_data = ppdev;

	return 0;
}

static int pop_dev_release(struct inode *inode, struct file *filp)
{
	struct pop_dev *ppdev = (struct pop_dev *)filp->private_data;

	atomic_dec(&ppdev->refcnt);
	filp->private_data = NULL;
	return 0;
}


static int pop_dev_mem_fault(struct vm_fault *vmf)
{
	struct pop_dev *ppdev;
	struct vm_area_struct *vma = vmf->vma;
	struct page *page;
	unsigned long pagenum = vmf->pgoff;
	unsigned long pa, pfn;

	if (unlikely(!vmf->vma->vm_file)) {
		pr_err("%s: vmf->vma->vm_file is NULL\n", __func__);
		return -EINVAL;
	}

	ppdev = vmf->vma->vm_file->private_data;
	if (unlikely(!ppdev)) {
		pr_err("%s: vmf->vma->vm_file->private_data (pop_dev) is NULL",
		       __func__);
		return -EINVAL;
	}

	pr_debug("%s: vma->vm_pgoff=%ld, vmf->pgoff=%ld\n",
		__func__, vma->vm_pgoff, vmf->pgoff);
	pr_debug("%s: page number %ld\n", __func__, pagenum);

	pa = virt_to_phys(ppdev->p2pmem + (pagenum << PAGE_SHIFT));
	pr_debug("%s: paddr of mapped p2pmem is %lx\n",
		__func__, pa);
	if (pa == 0) {
		pr_err("wrong pa\n");
		return VM_FAULT_SIGBUS;
	}

	pfn = pa >> PAGE_SHIFT;
	if (!pfn_valid(pfn)) {
		pr_err("invalid pfn %lx\n", pfn);
		return VM_FAULT_SIGBUS;
	}

	page = pfn_to_page(pfn);
	get_page(page);
	vmf->page = page;

	return 0;
}

static const struct vm_operations_struct pop_dev_mmap_ops = {
	.fault	= pop_dev_mem_fault,
};

static int pop_dev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct pop_dev *ppdev = filp->private_data;
	unsigned long off = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long len = vma->vm_end - vma->vm_start;

	if (!ppdev) {
		pr_err("%s: filp->private_data is NULL\n", __func__);
		return -EINVAL;
	}

	pr_info("%s: offset is %lu, length is %lu\n", __func__, off, len);
	if (off + len > ppdev->size) {
		pr_err("%s: len %lu is larger than p2pmem size %lu of %s\n",
		       __func__, len,  ppdev->size, ppdev->devname);
		return -ENOMEM;
	}

	vma->vm_ops = &pop_dev_mmap_ops;

	return 0;
}

static const struct file_operations pop_dev_fops = {
	.owner		= THIS_MODULE,
	.mmap		= pop_dev_mmap,
	.open		= pop_dev_open,
	.release	= pop_dev_release,
};

static int pop_register_p2pmem(struct pci_dev *pdev, size_t size)
{
	/* allocate 'all' p2pmem from pdev, register miscdevice
	 * for the p2pmem, and register pop_dev to pop.dev_list.
	 * Return value is allocated size;
	 */

	int ret;
	void *p2pmem;
	struct pop_dev *ppdev;

	if ((ppdev = pop_find_dev(pdev))) {
		pr_warn("device %s is already registered\n", pci_name(pdev));
		return ppdev->size;
	}

	/* if reg.size is 0, allocate all the p2pmem */
	if (size == 0)
		if (pdev->p2pdma->pool)
			size = gen_pool_size(pdev->p2pdma->pool);

	p2pmem = pci_alloc_p2pmem(pdev, size);
	if (!p2pmem) {
		pr_err("failed to alloc %luB p2pmem from %s\n",
		       size, pci_name(pdev));
		return -ENOMEM;
	}

	/* allocte pop_dev structure and register it to struct pop */
	ppdev = kmalloc(sizeof(*ppdev), GFP_KERNEL);
	if (!ppdev) {
		pci_free_p2pmem(pdev, p2pmem, size);
		return -ENOMEM;
	}

	memset(ppdev, 0, sizeof(*ppdev));
	INIT_LIST_HEAD(&ppdev->list);
	snprintf(ppdev->devname, DEVNAMELEN, "pop/%s", pci_name(pdev));
	ppdev->pdev		= pdev;
	ppdev->p2pmem		= p2pmem;
	ppdev->size		= size;
	ppdev->mdev.minor	= MISC_DYNAMIC_MINOR;
	ppdev->mdev.fops	= &pop_dev_fops;
	ppdev->mdev.name	= ppdev->devname;
	atomic_set(&ppdev->refcnt, 0);

	ret = misc_register(&ppdev->mdev);
	if (ret) {
		pr_err("failed to register %s\n", ppdev->devname);
		pci_free_p2pmem(pdev, p2pmem, size);
		return -EINVAL;
	}

	list_add_tail(&ppdev->list, &pop.dev_list);

	pr_info("/dev/%s with %luB p2pmem registered\n",
		ppdev->devname, ppdev->size);
	return size;
}

static void pop_unregister_p2pmem(struct pop_dev *ppdev)
{
	misc_deregister(&ppdev->mdev);
	pci_free_p2pmem(ppdev->pdev, ppdev->p2pmem, ppdev->size);
	pci_dev_put(ppdev->pdev);
	list_del(&ppdev->list);

	pr_info("/dev/%s unregistered\n", ppdev->devname);
	kfree(ppdev);
}


static int pop_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int pop_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static long pop_ioctl(struct file *filp, unsigned int cmd, unsigned long data)
{
	int ret = 0, size;
	struct pop_p2pmem_reg reg;
	struct pci_dev *pdev;
	struct pop_dev *ppdev;

	switch (cmd) {
	case POP_P2PMEM_REG:
		if (copy_from_user(&reg, (void *)data, sizeof(reg)) != 0) {
			pr_err("%s: copy_from_user failed\n", __func__);
			return -EFAULT;
		}

		pdev = pci_get_domain_bus_and_slot(reg.domain, reg.bus,
						   PCI_DEVFN(reg.slot,
							     reg.func));
		if (!pdev) {
			pr_err("no pci dev %04x:%02x:%02x:%x found\n",
			       reg.domain, reg.bus, reg.slot, reg.func);
			return -ENODEV;
		}

		if (!pci_has_p2pmem(pdev)) {
			pr_err("%04x:%02x:%02x:%x does not support p2pmem\n",
			       reg.domain, reg.bus, reg.slot, reg.func);
			ret = -EINVAL;
			goto dev_put_out;
		}

		size = pop_register_p2pmem(pdev, reg.size);
		if (size < 0)
			goto dev_put_out;

		reg.size = size;
		if (copy_to_user((void *)data, &reg, sizeof(reg)) != 0) {
			pr_err("%s: copy_to_user failed\n", __func__);
			ret = -EFAULT;
			goto dev_put_out;
		}

		ret = 0;
		break;

	case POP_P2PMEM_UNREG:
		if (copy_from_user(&reg, (void *)data, sizeof(reg)) != 0) {
			pr_err("%s: copy_from_user failed\n", __func__);
			return -EFAULT;
		}

		pdev = pci_get_domain_bus_and_slot(reg.domain, reg.bus,
						   PCI_DEVFN(reg.slot,
							     reg.func));
		if (!pdev) {
			pr_err("no pci dev %04x:%02x:%02x:%x found\n",
			       reg.domain, reg.bus, reg.slot, reg.func);
			return -ENODEV;
		}

		ppdev = pop_find_dev(pdev);
		if (!ppdev) {
			pr_err("pci dev %04x:%02x:%02x:%x is not registered\n",
			       reg.domain, reg.bus, reg.slot, reg.func);
			ret = -ENODEV;
			goto dev_put_out;
		}

		pop_unregister_p2pmem(ppdev);
		break;

	default:
		pr_err("invalid ioctl dommand: %d\n", cmd);
		return -EINVAL;
	}

	return ret;
dev_put_out:
	pci_dev_put(pdev);
	return ret;
}


static const struct file_operations pop_fops = {
	.owner		= THIS_MODULE,
	.open		= pop_open,
	.release	= pop_release,
	.unlocked_ioctl	= pop_ioctl,
};

static struct miscdevice pop_mdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "boogiepop",
	.fops	= &pop_fops,
};

static int __init pop_init(void)
{
	int ret = 0;

	/* initialize pop structure */
	memset(&pop, 0, sizeof(pop));
	INIT_LIST_HEAD(&pop.dev_list);

	ret = misc_register(&pop_mdev);
	if (ret) {
		pr_err("failed to register miscdevice for pop\n");
		goto err_out;
	}

	pr_info("%s (v%s) is loaded\n", KBUILD_MODNAME, POP_VERSION);

err_out:
	return ret;
}

static void __exit pop_exit(void)
{
	struct list_head *pos, *tmp;
	struct pop_dev *ppdev;

	/* unrgigster p2pmem */
	list_for_each_safe(pos, tmp, &pop.dev_list) {
		ppdev = list_entry(pos, struct pop_dev, list);
		pop_unregister_p2pmem(ppdev);
	}

	/* unregister /dev/pop/pop */
	misc_deregister(&pop_mdev);

	pr_info("%s (v%s) is unloaded\n", KBUILD_MODNAME, POP_VERSION);
}

module_init(pop_init);
module_exit(pop_exit);
MODULE_AUTHOR("Ryo Nakamura <upa@haeena.net>");
MODULE_LICENSE("GPL");
MODULE_VERSION(POP_VERSION);
