#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VIEWCOMPILEMANAGER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VIEWCOMPILEMANAGER_H

#include <vsg/app/CompileManager.h>
#include <vsg/app/View.h>
#include <vsg/vk/Context.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace RenderVsg
{
    // VSG 1.1.15 exposes registration but not removal of persistent contexts.
    // Use its existing traversal queue, pools and compilation implementation;
    // only add ownership of contexts created for transient map/preview Views.
    class ViewCompileManager final : public vsg::Inherit<vsg::CompileManager, ViewCompileManager>
    {
    public:
        ViewCompileManager(vsg::Viewer& viewer, vsg::ref_ptr<vsg::ResourceHints> hints)
            : Inherit(viewer, hints)
        {
        }

        struct Registration
        {
            vsg::ref_ptr<ViewCompileManager> manager;
            // Own the View until every raw Context::viewDependentState pointer
            // has been unregistered, including during rollback/destruction.
            vsg::ref_ptr<vsg::View> view;
            std::vector<vsg::ref_ptr<vsg::Context>> contexts;

            Registration(vsg::ref_ptr<ViewCompileManager> owner, vsg::ref_ptr<vsg::View> source)
                : manager(std::move(owner)), view(std::move(source))
            {
            }
            Registration(const Registration&) = delete;
            Registration& operator=(const Registration&) = delete;
            ~Registration() { manager->removeContexts(contexts); }
        };

        [[nodiscard]] std::unique_ptr<Registration> registerFramebufferView(
            vsg::Framebuffer& framebuffer, vsg::ref_ptr<vsg::View> view)
        {
            auto registration = std::make_unique<Registration>(vsg::ref_ptr<ViewCompileManager>(this), view);
            TraversalLease lease(*this);
            std::vector<std::size_t> originalSizes;
            originalSizes.reserve(lease.traversals.size());
            for (const auto& traversal : lease.traversals)
                originalSizes.push_back(traversal->contexts.size());
            try
            {
                std::size_t i = 0;
                for (auto& traversal : lease.traversals)
                {
                    traversal->add(framebuffer, view);
                    auto firstAdded = std::next(traversal->contexts.begin(), originalSizes[i++]);
                    registration->contexts.insert(
                        registration->contexts.end(), firstAdded, traversal->contexts.end());
                }
            }
            catch (...)
            {
                // add() can append the parent context before a shadow/context
                // allocation fails. Roll back the entire appended range.
                std::size_t i = 0;
                for (auto& traversal : lease.traversals)
                    traversal->contexts.resize(originalSizes[i++]);
                throw;
            }
            return registration;
        }

        // Read-only QC inventory, also used to verify repeated map transitions
        // do not accumulate contexts after the auxiliary image is retired.
        [[nodiscard]] std::size_t contextCount()
        {
            TraversalLease lease(*this);
            std::size_t count = 0;
            for (const auto& traversal : lease.traversals)
                count += traversal->contexts.size();
            return count;
        }

    private:
        struct TraversalLease
        {
            ViewCompileManager& manager;
            CompileTraversals::container_type traversals;
            explicit TraversalLease(ViewCompileManager& owner)
                : manager(owner), traversals(owner.takeCompileTraversals(owner.numCompileTraversals))
            {
            }
            ~TraversalLease()
            {
                for (auto& traversal : traversals)
                    manager.compileTraversals->add(traversal);
            }
        };

        void removeContexts(const std::vector<vsg::ref_ptr<vsg::Context>>& contexts)
        {
            if (contexts.empty())
                return;
            TraversalLease lease(*this);
            for (auto& traversal : lease.traversals)
                traversal->contexts.remove_if([&](const auto& candidate) {
                    return std::find(contexts.begin(), contexts.end(), candidate) != contexts.end();
                });
        }
    };
}

#endif
