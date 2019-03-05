/* pop.c */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/pci-p2pdma.h>

#include <libpop.h>

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
	char	devname[DEVNAMELEN];	/* /dev/pop/BUS:SLOT.FUNC	*/
	void	*p2pmem;     	/* p2pmem of the above pci dev		*/
	size_t	size;		/* p2pmem size	*/

	struct miscdevice	mdev;	/* char dev for this pop_dev */
	atomic_t       		refcnt;
};

/* structure describing libpop kernel module */
struct pop {
	struct list_head	dev_list;	/* list of pop_dev */
};
static struct pop pop;

static struct pop_dev *pop_find_dev(struct pci_dev *pdev) {
	struct pop_dev *ppdev;

	list_for_each_entry(ppdev, &pop.dev_list, list) {
		if (ppdev->pdev == pdev)
			return ppdev;
	}

	return NULL;
}

static struct file_operations pop_dev_fops = {
	.owner		= THIS_MODULE,
};

static int pop_register_p2pmem(struct pci_dev *pdev, size_t size) {

	/* allocate 'size'-byte p2pmem from pdev, register miscdevice
	 * for the p2pmem, and register pop_dev to pop.dev_list */

	int ret;
	void *p2pmem;
	struct pop_dev *ppdev;
	
	if (pop_find_dev(pdev)) {
		pr_err("device %02x:%02x.%x is already registered\n",
		       pdev->bus->number, PCI_SLOT(pdev->devfn),
		       PCI_SLOT(pdev->devfn));
		return -EINVAL;
	}
			
	p2pmem = pci_alloc_p2pmem(pdev, size);
	if (!p2pmem) {
		pr_err("failed to alloc %lu-byte p2pmem from  %02x:%02x.%x\n",
		       size, pdev->bus->number, PCI_SLOT(pdev->devfn),
		       PCI_FUNC(pdev->devfn));
		return -ENOMEM;
	}

	/* allocte pop_dev structure and register it to struct pop */
	ppdev = (struct pop_dev *)kmalloc(sizeof(*ppdev), GFP_KERNEL);
	if (!ppdev) {
		pr_err("failed to allocate pop_dev structure\n");
		pci_free_p2pmem(pdev, p2pmem, size);
		return -ENOMEM;
	}

	memset(ppdev, 0, sizeof(*ppdev));
	INIT_LIST_HEAD(&ppdev->list);
	snprintf(ppdev->devname, DEVNAMELEN, "pop/%02x:%02x.%x",
		 pdev->bus->number, PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));
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

	pr_info("/dev/%s with %lu-byte p2pmem registered\n",
		ppdev->devname, ppdev->size);

	return 0;
}

static void pop_unregister_p2pmem(struct pop_dev *ppdev) {
	
	misc_deregister(&ppdev->mdev);
	pci_free_p2pmem(ppdev->pdev, ppdev->p2pmem, ppdev->size);
	pci_dev_put(ppdev->pdev);
	list_del(&ppdev->list);

	pr_info("/dev/%s unregistered\n", ppdev->devname);
	kfree(ppdev);
}


static int pop_open(struct inode *inode, struct file *filp) {
	pr_info("%s\n", __func__);
	return 0;
}

static int pop_release(struct inode *inode, struct file *filp) {
	pr_info("%s\n", __func__);
	return 0;
}

static long pop_ioctl(struct file *filp,
		      unsigned int cmd, unsigned long data) {
	
	int ret = 0;
	struct pop_p2pmem_reg reg;
	struct pci_dev *pdev;
	struct pop_dev *ppdev;

	switch(cmd) {
	case POP_P2PMEM_REG:
		if (copy_from_user(&reg, (void *)data, sizeof(reg)) != 0) {
			pr_err("%s: copy_from_user failed\n", __func__);
			return -EFAULT;
		}

		if (reg.size < (1 << PAGE_SHIFT) ||
		    reg.size % (1 << PAGE_SHIFT) != 0) {
			pr_err("size must be power of %d\n", 1 << PAGE_SHIFT);
			return -EINVAL;
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
		
		ret = pop_register_p2pmem(pdev, reg.size);
		if (ret) 
			goto dev_put_out;
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
	

static struct file_operations pop_fops = {
	.owner		= THIS_MODULE,
	.open		= pop_open,
	.release	= pop_release,
	.unlocked_ioctl	= pop_ioctl,
};

static struct miscdevice pop_mdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "pop/pop",
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
