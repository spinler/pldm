#pragma once

#include "config.h"

#include "libpldm/fru.h"
#include "libpldm/pdr.h"

#include "common/utils.hpp"
#include "fru_parser.hpp"
#include "libpldmresponder/pdr_utils.hpp"
#include "oem_handler.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "pldmd/handler.hpp"
#include "requester/handler.hpp"

#include <sdbusplus/message.hpp>

#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace pldm::utils;
using namespace pldm::dbus_api;
namespace pldm
{

namespace responder
{

namespace dbus
{

using Value =
    std::variant<bool, uint8_t, int16_t, uint16_t, int32_t, uint32_t, int64_t,
                 uint64_t, double, std::string, std::vector<uint8_t>>;
using PropertyMap = std::map<Property, Value>;
using InterfaceMap = std::map<Interface, PropertyMap>;
using ObjectValueTree = std::map<sdbusplus::message::object_path, InterfaceMap>;
using ObjectPath = std::string;
using AssociatedEntityMap = std::map<ObjectPath, pldm_entity>;
using ObjectPathToRSIMap = std::map<ObjectPath, uint16_t>;

} // namespace dbus

using ChangeEntry = uint32_t;

/** @class FruImpl
 *
 *  @brief Builds the PLDM FRU table containing the FRU records
 */
class FruImpl
{
  public:
    /* @brief Header size for FRU record, it includes the FRU record set
     *        identifier, FRU record type, Number of FRU fields, Encoding type
     *        of FRU fields
     */
    static constexpr size_t recHeaderSize =
        sizeof(struct pldm_fru_record_data_format) -
        sizeof(struct pldm_fru_record_tlv);

    /** @brief Constructor for FruImpl, the configPath is consumed to build the
     *         FruParser object.
     *
     *  @param[in] configPath - path to the directory containing config files
     *                          for PLDM FRU
     *  @param[in] fruMasterJsonPath - path to the file containing the FRU D-Bus
     *                                 Lookup Map
     *  @param[in] pdrRepo - opaque pointer to PDR repository
     *  @param[in] entityTree - opaque pointer to the entity association tree
     *  @param[in] bmcEntityTree - opaque pointer to bmc's entity association
     *                             tree
     *  @param[in] oemFruHandler - OEM fru handler
     *  @param[in] requester - PLDM Requester reference
     *  @param[in] handler - PLDM request handler
     *  @param[in] mctp_eid - MCTP eid of Host
     *  @param[in] event - reference of main event loop of pldmd
     */
    FruImpl(const std::string& configPath,
            const std::filesystem::path& fruMasterJsonPath, pldm_pdr* pdrRepo,
            pldm_entity_association_tree* entityTree,
            pldm_entity_association_tree* bmcEntityTree,
            pldm::responder::oem_fru::Handler* oemFruHandler,
            Requester& requester,
            pldm::requester::Handler<pldm::requester::Request>* handler,
            uint8_t mctp_eid, sdeventplus::Event& event) :
        parser(configPath, fruMasterJsonPath),
        pdrRepo(pdrRepo), entityTree(entityTree), bmcEntityTree(bmcEntityTree),
        oemFruHandler(oemFruHandler), requester(requester), handler(handler),
        mctp_eid(mctp_eid), event(event)
    {
        startStateSensorId = 0;
        startStateEffecterId = 0;
        static constexpr auto inventoryObjPath =
            "/xyz/openbmc_project/inventory/system/chassis";
        static constexpr auto itemInterface =
            "xyz.openbmc_project.Inventory.Item";
        static constexpr auto fanInterface =
            "xyz.openbmc_project.Inventory.Item.Fan";
        static constexpr auto psuInterface =
            "xyz.openbmc_project.Inventory.Item.PowerSupply";
        static constexpr auto pcieAdapterInterface =
            "xyz.openbmc_project.Inventory.Item.PCIeDevice";
        subscribeFruPresence(inventoryObjPath, fanInterface, itemInterface,
                             fanHotplugMatch);
        subscribeFruPresence(inventoryObjPath, psuInterface, itemInterface,
                             psuHotplugMatch);
        subscribeFruPresence(inventoryObjPath, pcieAdapterInterface,
                             itemInterface, pcieHotplugMatch);
    }

    /** @brief Total length of the FRU table in bytes, this excludes the pad
     *         bytes and the checksum.
     *
     *  @return size of the FRU table
     */
    uint32_t size() const
    {
        return table.size() - padBytes;
    }

    /** @brief The checksum of the contents of the FRU table
     *
     *  @return checksum
     */
    uint32_t checkSum() const
    {
        return checksum;
    }

    /** @brief Number of record set identifiers in the FRU tables
     *
     *  @return number of record set identifiers
     */
    uint16_t numRSI() const
    {
        return rsi;
    }

    /** @brief The number of FRU records in the table
     *
     *  @return number of FRU records
     */
    uint16_t numRecords() const
    {
        return numRecs;
    }

    /** @brief Get the FRU table
     *
     *  @param[out] - Populate response with the FRU table
     */
    void getFRUTable(Response& response);

    /** @brief Get FRU Record Table By Option
     *  @param[out] response - Populate response with the FRU table got by
     *                         options
     *  @param[in] fruTableHandle - The fru table handle
     *  @param[in] recordSetIdentifer - The record set identifier
     *  @param[in] recordType - The record type
     *  @param[in] fieldType - The field type
     */
    int getFRURecordByOption(Response& response, uint16_t fruTableHandle,
                             uint16_t recordSetIdentifer, uint8_t recordType,
                             uint8_t fieldType);

    /** @brief FRU table is built by processing the D-Bus inventory namespace
     *         based on the config files for FRU. The table is populated based
     *         on the isBuilt flag.
     */
    void buildFRUTable();

    /** @brief Get std::map associated with the entity
     *         key: object path
     *         value: pldm_entity
     *
     *  @return std::map<ObjectPath, pldm_entity>
     */
    inline const pldm::responder::dbus::AssociatedEntityMap&
        getAssociateEntityMap() const
    {
        return associatedEntityMap;
    }

    /** @brief Get pldm entity by the object path
     *
     *  @param[in] objects - std::map The object value tree
     *  @param[in] path    - Object path
     *
     *  @return pldm_entity
     */
    pldm_entity getEntityByObjectPath(const dbus::ObjectValueTree& objects,
                                      const std::string& path);

    /** @brief Update pldm entity to association tree
     *
     *  @param[in] objects - std::map The object value tree
     *  @param[in] path    - Object path
     *
     */
    void updateAssociationTree(const dbus::ObjectValueTree& objects,
                               const std::string& path);

    /* @brief Method to populate the firmware version ID
     *
     * @return firmware version ID
     */
    std::string populatefwVersion();

    /* @brief set FRU Record Table
     *
     * @param[in] fruData - the data of the fru
     *
     */
    int setFRUTable(const std::vector<uint8_t>& fruData);

    /* @brief Send a PLDM event to host firmware containing a list of record
     *        handles of PDRs that the host firmware has to fetch.
     * @param[in] pdrRecordHandles - list of PDR record handles
     * @param[in] eventDataOps - event data operation for PDRRepositoryChgEvent
     *                           in DSP0248
     */
    void sendPDRRepositoryChgEventbyPDRHandles(
        std::vector<uint32_t>&& pdrRecordHandles,
        std::vector<uint8_t>&& eventDataOps);

    std::vector<uint32_t> setStatePDRParams(
        const std::string& pdrJsonsDir, uint16_t nextSensorId,
        uint16_t nextEffecterId,
        pldm::responder::pdr_utils::DbusObjMaps& sensorDbusObjMaps,
        pldm::responder::pdr_utils::DbusObjMaps& effecterDbusObjMaps,
        bool hotPlug, const Json& json, const std::string& fruObjectPath = "",
        pldm::responder::pdr_utils::Type pdrType = 0);

  private:
    uint16_t nextRSI()
    {
        return ++rsi;
    }

    uint32_t nextRecordHandle()
    {
        return ++rh;
    }

    uint32_t rh = 0;
    uint16_t rsi = 0;
    uint16_t numRecs = 0;
    uint8_t padBytes = 0;
    std::vector<uint8_t> table;
    uint32_t checksum = 0;
    bool isBuilt = false;

    fru_parser::FruParser parser;
    pldm_pdr* pdrRepo;
    pldm_entity_association_tree* entityTree;
    pldm_entity_association_tree* bmcEntityTree;
    pldm::responder::oem_fru::Handler* oemFruHandler;
    Requester& requester;
    pldm::requester::Handler<pldm::requester::Request>* handler;
    uint8_t mctp_eid;
    sdeventplus::Event& event;

    std::map<dbus::ObjectPath, pldm_entity_node*> objToEntityNode{};
    dbus::ObjectPathToRSIMap objectPathToRSIMap{};

    /** @brief populateRecord builds the FRU records for an instance of FRU and
     *         updates the FRU table with the FRU records.
     *
     *  @param[in] interfaces - D-Bus interfaces and the associated property
     *                          values for the FRU
     *  @param[in] recordInfos - FRU record info to build the FRU records
     *  @param[in/out] entity - PLDM entity corresponding to FRU instance
     *  @param[in] objectPath - FRU object path
     *  @param[in] concurrentAdd - whether this is a CM operation
     *
     *  @return uint32_t the newly added PDR record handle
     */
    uint32_t populateRecords(const dbus::InterfaceMap& interfaces,
                             const fru_parser::FruRecordInfos& recordInfos,
                             const pldm_entity& entity,
                             const dbus::ObjectPath& objectPath,
                             bool concurrentAdd = false);

    /** @brief subscribeFruPresence subscribes for the "Present" property
     *         change signal. This enables pldm to know when a fru is
     *         added or removed.
     *  @param[in] inventoryObjPath - the inventory object path for chassis
     *  @param[in] fruInterface - the fru interface to look for
     *  @param[in] itemInterface - the inventory item interface
     *  @param[in] fruHotPlugMatch - D-Bus property changed signal match
     *                               for the fru
     */
    void subscribeFruPresence(
        const std::string& inventoryObjPath, const std::string& fruInterface,
        const std::string& itemInterface,
        std::vector<std::unique_ptr<sdbusplus::bus::match::match>>&
            fruHotPlugMatch);

    /** @brief processFruPresenceChange processes the "Present" property change
     *         signal for a fru.
     *  @param[in] chProperties - list of properties which have changed
     *  @param[in] fruObjPath - fru object path
     *  @param[in] fruInterface - fru interface
     */

    void processFruPresenceChange(const DbusChangedProps& chProperties,
                                  const std::string& fruObjPath,
                                  const std::string& fruInterface);

    /** @brief Builds a FRU record set PDR and associted PDRs after a
     *         concurrent add operation.
     *  @param[in] fruInterface - the FRU interface
     *  @param[in] fruObjectPath - the FRU object path
     *
     *  @return none
     */
    void buildIndividualFRU(const std::string& fruInterface,
                            const std::string& fruObjectPath);

    /** @brief Deletes a FRU record set PDR and it's associted PDRs after
     *         a concurrent remove operation.
     *  @param[in] fruObjectPath - the FRU object path
     *  @return none
     */
    void removeIndividualFRU(const std::string& fruObjPath);

    void reGenerateStatePDR(const std::string& fruObjectPath,
                            std::vector<uint32_t>& recordHdlList);

    uint32_t addHotPlugRecord(pldm::responder::pdr_utils::PdrEntry pdrEntry);

    /** @brief Associate sensor/effecter to FRU entity
     */
    dbus::AssociatedEntityMap associatedEntityMap;

    /** @brief vectors to catch the D-Bus property change signals for the frus
     */
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> fanHotplugMatch;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> psuHotplugMatch;
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> pcieHotplugMatch;
    dbus::ObjectValueTree objects;
    std::string statePDRJsonsDir;
    uint16_t startStateSensorId;
    uint16_t startStateEffecterId;
};

namespace fru
{

class Handler : public CmdHandler
{

  public:
    Handler(const std::string& configPath,
            const std::filesystem::path& fruMasterJsonPath, pldm_pdr* pdrRepo,
            pldm_entity_association_tree* entityTree,
            pldm_entity_association_tree* bmcEntityTree,
            pldm::responder::oem_fru::Handler* oemFruHandler,
            Requester& requester,
            pldm::requester::Handler<pldm::requester::Request>* handler,
            uint8_t mctp_eid, sdeventplus::Event& event) :
        impl(configPath, fruMasterJsonPath, pdrRepo, entityTree, bmcEntityTree,
             oemFruHandler, requester, handler, mctp_eid, event)
    {
        handlers.emplace(PLDM_GET_FRU_RECORD_TABLE_METADATA,
                         [this](const pldm_msg* request, size_t payloadLength) {
                             return this->getFRURecordTableMetadata(
                                 request, payloadLength);
                         });

        handlers.emplace(PLDM_GET_FRU_RECORD_TABLE,
                         [this](const pldm_msg* request, size_t payloadLength) {
                             return this->getFRURecordTable(request,
                                                            payloadLength);
                         });
        handlers.emplace(PLDM_GET_FRU_RECORD_BY_OPTION,
                         [this](const pldm_msg* request, size_t payloadLength) {
                             return this->getFRURecordByOption(request,
                                                               payloadLength);
                         });
        handlers.emplace(PLDM_SET_FRU_RECORD_TABLE,
                         [this](const pldm_msg* request, size_t payloadLength) {
                             return this->setFRURecordTable(request,
                                                            payloadLength);
                         });
    }

    /** @brief Handler for Get FRURecordTableMetadata
     *
     *  @param[in] request - Request message payload
     *  @param[in] payloadLength - Request payload length
     *
     *  @return PLDM response message
     */
    Response getFRURecordTableMetadata(const pldm_msg* request,
                                       size_t payloadLength);

    /** @brief Handler for GetFRURecordTable
     *
     *  @param[in] request - Request message payload
     *  @param[in] payloadLength - Request payload length
     *
     *  @return PLDM response message
     */
    Response getFRURecordTable(const pldm_msg* request, size_t payloadLength);

    /** @brief Build FRU table is bnot already built
     *
     */
    void buildFRUTable()
    {
        impl.buildFRUTable();
    }

    /** @brief Get std::map associated with the entity
     *         key: object path
     *         value: pldm_entity
     *
     *  @return std::map<ObjectPath, pldm_entity>
     */
    const pldm::responder::dbus::AssociatedEntityMap&
        getAssociateEntityMap() const
    {
        return impl.getAssociateEntityMap();
    }

    /** @brief Handler for GetFRURecordByOption
     *
     *  @param[in] request - Request message payload
     *  @param[in] payloadLength - Request payload length
     *
     *  @return PLDM response message
     */
    Response getFRURecordByOption(const pldm_msg* request,
                                  size_t payloadLength);

    /** @brief Handler for SetFRURecordTable
     *
     *  @param[in] request - Request message
     *  @param[in] payloadLength - Request payload length
     *
     *  @return PLDM response message
     */
    Response setFRURecordTable(const pldm_msg* request, size_t payloadLength);

    void setStatePDRParams(
        const std::string& pdrJsonsDir, uint16_t nextSensorId,
        uint16_t nextEffecterId,
        pldm::responder::pdr_utils::DbusObjMaps& sensorDbusObjMaps,
        pldm::responder::pdr_utils::DbusObjMaps& effecterDbusObjMaps,
        bool hotPlug);

    // std::vector<uint8_t> table;
    using Table = std::vector<uint8_t>;

  private:
    FruImpl impl;
};

} // namespace fru

} // namespace responder

} // namespace pldm
