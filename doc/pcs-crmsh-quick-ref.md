<!-- (new version of doctoc removed this, so added above:) -->
<!-- *generated with [DocToc](https://github.com/thlorenz/doctoc-web/)* -->
<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->
**Table of Contents**

- [General Operations](#general-operations)
  - [Display the configuration](#display-the-configuration)
  - [Display the current status](#display-the-current-status)
  - [Node standby](#node-standby)
  - [Set cluster property](#set-cluster-property)
- [Resource manipulation](#resource-manipulation)
  - [List Resource Agent (RA) classes](#list-resource-agent-ra-classes)
  - [List available RAs](#list-available-ras)
  - [List RA info](#list-ra-info)
  - [Create a resource](#create-a-resource)
  - [Display a resource](#display-a-resource)
  - [Display fencing resources](#display-fencing-resources)
  - [Display Stonith RA info](#display-stonith-ra-info)
  - [Start a resource](#start-a-resource)
  - [Stop a resource](#stop-a-resource)
  - [Remove a resource](#remove-a-resource)
  - [Modify a resource](#modify-a-resource)
  - [Delete parameters for a given resource](#delete-parameters-for-a-given-resource)
  - [List the current resource defaults](#list-the-current-resource-defaults)
  - [Set resource defaults](#set-resource-defaults)
  - [List the current operation defaults](#list-the-current-operation-defaults)
  - [Set operation defaults](#set-operation-defaults)
  - [Set Colocation](#set-colocation)
  - [Set ordering](#set-ordering)
  - [Set preferred location](#set-preferred-location)
  - [Move resources](#move-resources)
  - [Resource tracing](#resource-tracing)
  - [Clear fail counts](#clear-fail-counts)
  - [Edit fail counts](#edit-fail-counts)
  - [Handling configuration elements by type](#handling-configuration-elements-by-type)
  - [Create a clone](#create-a-clone)
  - [Create a master/slave clone](#create-a-masterslave-clone)
- [Other operations](#other-operations)
  - [Batch changes](#batch-changes)
  - [Template creation](#template-creation)
  - [Log analysis](#log-analysis)
  - [Configuration scripts](#configuration-scripts)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

# General Operations

## Display the configuration

    crmsh # crm configure show xml
    pcs   # pcs cluster cib

To show a simplified (non-xml) syntax

    crmsh # crm configure show
    pcs   # pcs config
    
## Display the current status

    crmsh # crm status
    pcs   # pcs status

also

    # crm_mon -1

## Node standby

Put node in standby

    crmsh # crm node standby pcmk-1
    pcs   # pcs cluster standby pcmk-1

Remove node from standby

    crmsh # crm node online pcmk-1
    pcs   # pcs cluster unstandby pcmk-1

crm has the ability to set the status on reboot or forever. 
pcs can apply the change to all the nodes.

## Set cluster property

    crmsh # crm configure property stonith-enabled=false
    pcs   # pcs property set stonith-enabled=false

# Resource manipulation

## List Resource Agent (RA) classes

    crmsh # crm ra classes
    pcs   # pcs resource standards

## List available RAs

    crmsh # crm ra list ocf
    crmsh # crm ra list lsb
    crmsh # crm ra list service
    crmsh # crm ra list stonith
    pcs   # pcs resource agents ocf
    pcs   # pcs resource agents lsb
    pcs   # pcs resource agents service
    pcs   # pcs resource agents stonith
    pcs   # pcs resource agents

You can also filter by provider

    crmsh # crm ra list ocf pacemaker
    pcs   # pcs resource agents ocf:pacemaker

## List RA info

    crmsh # crm ra meta IPaddr2
    pcs   # pcs resource describe IPaddr2

Use any RA name (like IPaddr2) from the list displayed with the previous command
You can also use the full class:provider:RA format if multiple RAs with the same name are available :

    crmsh # crm ra meta ocf:heartbeat:IPaddr2
    pcs   # pcs resource describe ocf:heartbeat:IPaddr2

## Create a resource

    crmsh # crm configure primitive ClusterIP ocf:heartbeat:IPaddr2 \
            params ip=192.168.122.120 cidr_netmask=32 \
            op monitor interval=30s 
    pcs   # pcs resource create ClusterIP IPaddr2 ip=192.168.0.120 cidr_netmask=32

The standard and provider (`ocf:heartbeat`) are determined automatically since `IPaddr2` is unique.
The monitor operation is automatically created based on the agent's metadata.

## Display a resource

    crmsh # crm configure show
    pcs   # pcs resource show

crmsh also displays fencing resources. 
The result can be filtered by supplying a resource name (IE `ClusterIP`):

    crmsh # crm configure show ClusterIP
    pcs   # pcs resource show ClusterIP

crmsh also displays fencing resources. 

## Display fencing resources

    crmsh # crm resource show
    pcs   # pcs stonith show

pcs treats STONITH devices separately.

## Display Stonith RA info

    crmsh # crm ra meta stonith:fence_ipmilan
    pcs   # pcs stonith describe fence_ipmilan

## Start a resource

    crmsh # crm resource start ClusterIP
    pcs   # pcs resource enable ClusterIP

## Stop a resource

    crmsh # crm resource stop ClusterIP
    pcs   # pcs resource disable ClusterIP

## Remove a resource

    crmsh # crm configure delete ClusterIP
    pcs   # pcs resource delete ClusterIP

## Modify a resource

    crmsh # crm resource param ClusterIP set clusterip_hash=sourceip
    pcs   # pcs resource update ClusterIP clusterip_hash=sourceip

crmsh also has an `edit` command which edits the simplified CIB syntax
(same commands as the command line) via a configurable text editor.

    crmsh # crm configure edit ClusterIP

Using the interactive shell mode of crmsh, multiple changes can be
edited and verified before committing to the live configuration.

    crmsh # crm configure
    crmsh # edit
    crmsh # verify
    crmsh # commit

## Delete parameters for a given resource

    crmsh # crm resource param ClusterIP delete nic
    pcs   # pcs resource update ClusterIP ip=192.168.0.98 nic=  

## List the current resource defaults

    crmsh # crm configure show type:rsc_defaults
    pcs   # pcs resource rsc defaults

## Set resource defaults

    crmsh # crm configure rsc_defaults resource-stickiness=100
    pcs   # pcs resource rsc defaults resource-stickiness=100
    
## List the current operation defaults

    crmsh # crm configure show type:op_defaults
    pcs   # pcs resource op defaults

## Set operation defaults

    crmsh # crm configure op_defaults timeout=240s
    pcs   # pcs resource op defaults timeout=240s

## Set Colocation

    crmsh # crm configure colocation website-with-ip INFINITY: WebSite ClusterIP
    pcs   # pcs constraint colocation add ClusterIP with WebSite INFINITY

With roles

    crmsh # crm configure colocation another-ip-with-website inf: AnotherIP WebSite:Master
    pcs   # pcs constraint colocation add Started AnotherIP with Master WebSite INFINITY

## Set ordering

    crmsh # crm configure order apache-after-ip mandatory: ClusterIP WebSite
    pcs   # pcs constraint order ClusterIP then WebSite

With roles:

    crmsh # crm configure order ip-after-website Mandatory: WebSite:Master AnotherIP
    pcs   # pcs constraint order promote WebSite then start AnotherIP

## Set preferred location

    crmsh # crm configure location prefer-pcmk-1 WebSite 50: pcmk-1
    pcs   # pcs constraint location WebSite prefers pcmk-1=50
    
With roles:

    crmsh # crm configure location prefer-pcmk-1 WebSite rule role=Master 50: \#uname eq pcmk-1
    pcs   # pcs constraint location WebSite rule role=master 50 \#uname eq pcmk-1

## Move resources

    crmsh # crm resource move WebSite pcmk-1
    pcs   # pcs resource move WebSite pcmk-1
    
    crmsh # crm resource unmove WebSite
    pcs   # pcs resource clear WebSite

A resource can also be moved away from a given node:

    crmsh # crm resource ban Website pcmk-2
    pcs   # pcs resource ban Website pcmk-2

Remember that moving a resource sets a stickyness to -INF to a given node until unmoved    

## Resource tracing

    crmsh # crm resource trace Website

## Clear fail counts

    crmsh # crm resource cleanup Website
    pcs   # pcs resource cleanup Website

## Edit fail counts

    crmsh # crm resource failcount Website show pcmk-1
    crmsh # crm resource failcount Website set pcmk-1 100

## Handling configuration elements by type

pcs deals with constraints differently. These can be manipulated by the command above as well as the following and others

    pcs   # pcs constraint list --full
    pcs   # pcs constraint remove cli-ban-Website-on-pcmk-1

Removing a constraint in crmsh uses the same command as removing a
resource.

    crmsh # crm configure remove cli-ban-Website-on-pcmk-1

The `show` and `edit` commands in crmsh can be used to manage
resources and constraints by type:

    crmsh # crm configure show type:primitive
    crmsh # crm configure edit type:colocation

## Create a clone

    crmsh # crm configure clone WebIP ClusterIP meta globally-unique=true clone-max=2 clone-node-max=2
    pcs   # pcs resource clone ClusterIP globally-unique=true clone-max=2 clone-node-max=2

## Create a master/slave clone

    crmsh # crm configure ms WebDataClone WebData \
            meta master-max=1 master-node-max=1 \
            clone-max=2 clone-node-max=1 notify=true
    pcs   # pcs resource master WebDataClone WebData \
            master-max=1 master-node-max=1 \
            clone-max=2 clone-node-max=1 notify=true

# Other operations

## Batch changes

    crmsh # crm
    crmsh # cib new drbd_cfg
    crmsh # configure primitive WebData ocf:linbit:drbd params drbd_resource=wwwdata \
            op monitor interval=60s
    crmsh # configure ms WebDataClone WebData meta master-max=1 master-node-max=1 \
            clone-max=2 clone-node-max=1 notify=true
    crmsh # cib commit drbd_cfg
    crmsh # quit
.

    pcs   # pcs cluster cib drbd_cfg
    pcs   # pcs -f drbd_cfg resource create WebData ocf:linbit:drbd drbd_resource=wwwdata \
            op monitor interval=60s
    pcs   # pcs -f drbd_cfg resource master WebDataClone WebData master-max=1 master-node-max=1 \
            clone-max=2 clone-node-max=1 notify=true
    pcs   # pcs cluster push cib drbd_cfg

## Template creation

Create a resource template based on a list of primitives of the same
type

    crmsh # crm configure assist template ClusterIP AdminIP

## Log analysis

Display information about recent cluster events

    crmsh # crm history
    crmsh # peinputs
    crmsh # transition pe-input-10
    crmsh # transition log pe-input-10

## Configuration scripts

Create and apply multiple-step cluster configurations including
configuration of cluster resources

    crmsh # crm script show apache
    crmsh # crm script run apache \
        id=WebSite \
        install=true \
        virtual-ip:ip=192.168.0.15 \
        database:id=WebData \
        database:install=true
