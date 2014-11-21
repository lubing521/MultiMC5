/* Copyright 2013 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "QuickModBrowsePage.h"
#include "ui_QuickModBrowsePage.h"

#include <QSortFilterProxyModel>
#include <QIdentityProxyModel>
#include <QListView>
#include <QMessageBox>

#include "gui/dialogs/quickmod/QuickModInstallDialog.h"
#include "gui/dialogs/quickmod/QuickModAddFileDialog.h"
#include "gui/dialogs/quickmod/QuickModCreateFromInstanceDialog.h"
#include "gui/dialogs/NewInstanceDialog.h"
#include "gui/dialogs/CustomMessageBox.h"
#include "gui/dialogs/VersionSelectDialog.h"

#include "logic/InstanceFactory.h"
#include "logic/OneSixInstance.h"

#include "logic/quickmod/QuickModMetadata.h"
#include "logic/quickmod/InstancePackageList.h"
#include "logic/quickmod/QuickModModel.h"
#include "logic/quickmod/Transaction.h"
#include "logic/quickmod/QuickModVersionModel.h"
#include "logic/quickmod/QuickModImagesLoader.h"

#include "MultiMC.h"

template <typename T> bool intersectLists(const QList<T> &l1, const QList<T> &l2)
{
	for (const T &item : l1)
	{
		if (!l2.contains(item))
		{
			return false;
		}
	}

	return true;
}
bool listContainsSubstring(const QStringList &list, const QString &str)
{
	for (const QString &item : list)
	{
		if (item.contains(str, Qt::CaseInsensitive))
		{
			return true;
		}
	}
	return false;
}
template <typename T> T *findParent(const QObject *me)
{
	if (me == 0)
	{
		return 0;
	}
	if (qobject_cast<T *>(me->parent()))
	{
		return qobject_cast<T *>(me->parent());
	}
	return findParent<T>(me->parent());
}

class ModFilterProxyModel : public QSortFilterProxyModel
{
	Q_OBJECT
public:
	ModFilterProxyModel(QObject *parent = 0) : QSortFilterProxyModel(parent)
	{
		setSortRole(Qt::DisplayRole);
		setSortCaseSensitivity(Qt::CaseInsensitive);
	}

	void setSourceModel(QAbstractItemModel *model)
	{
		QSortFilterProxyModel::setSourceModel(model);
		connect(model, &QAbstractItemModel::modelReset, this, &ModFilterProxyModel::resort);
		connect(model, SIGNAL(rowsInserted(QModelIndex, int, int)), this, SLOT(resort()));
		resort();
	}

	void setTags(const QStringList &tags)
	{
		m_tags = tags;
		invalidateFilter();
	}
	void setCategory(const QString &category)
	{
		m_category = category;
		invalidateFilter();
	}
	void setMCVersion(const QString &version)
	{
		m_mcVersion = version;
		invalidateFilter();
		qDebug() << m_mcVersion;
	}
	void setFulltext(const QString &query)
	{
		m_fulltext = query;
		invalidateFilter();
	}

private slots:
	void resort()
	{
		sort(0);
	}

protected:
	bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const
	{
		const QModelIndex index = sourceModel()->index(source_row, 0, source_parent);

		if (!m_tags.isEmpty())
		{
			if (!intersectLists(m_tags, index.data(QuickModModel::TagsRole).toStringList()))
			{
				return false;
			}
		}
		if (!m_category.isEmpty())
		{
			if (!listContainsSubstring(index.data(QuickModModel::CategoriesRole).toStringList(),
									   m_category))
			{
				return false;
			}
		}
		if (!m_mcVersion.isEmpty())
		{
			auto acceptedVersions = index.data(QuickModModel::MCVersionsRole).toStringList();
			if (!Util::versionIsInInterval(Util::Version(m_mcVersion), acceptedVersions.join(',')))
			{
				return false;
			}
		}
		if (!m_fulltext.isEmpty())
		{
			bool inName = index.data(QuickModModel::NameRole).toString().contains(
				m_fulltext, Qt::CaseInsensitive);
			bool inDesc = index.data(QuickModModel::DescriptionRole).toString().contains(
				m_fulltext, Qt::CaseInsensitive);
			if (!inName && !inDesc)
			{
				return false;
			}
		}

		return true;
	}

private:
	QStringList m_tags;
	QString m_category;
	QString m_mcVersion;
	QString m_fulltext;
};

class TagsValidator : public QValidator
{
	Q_OBJECT
public:
	TagsValidator(QObject *parent = 0) : QValidator(parent)
	{
	}

protected:
	State validate(QString &input, int &pos) const
	{
		// TODO write a good validator
		return Acceptable;
	}
};

class CheckboxProxyModel : public QIdentityProxyModel
{
	Q_OBJECT
public:
	CheckboxProxyModel(QObject *parent) : QIdentityProxyModel(parent)
	{
	}

	void setSourceModel(std::shared_ptr<OneSixInstance> instance, QAbstractItemModel *model)
	{
		QIdentityProxyModel::setSourceModel(model);
		m_instance = instance;
		connect(instance.get(), &OneSixInstance::versionReloaded, this,
				&CheckboxProxyModel::update);
		update();
	}

	Qt::ItemFlags flags(const QModelIndex &index) const
	{
		return QIdentityProxyModel::flags(index) | Qt::ItemIsUserCheckable;
	}
	bool setData(const QModelIndex &index, const QVariant &value, int role)
	{
		if (index.isValid() && role == Qt::CheckStateRole)
		{
			if (value == Qt::Checked)
			{
				emit dataChanged(index, index, QVector<int>() << Qt::CheckStateRole);
				emit checkChanged(mapToSource(index), true);
				return true;
			}
			else if (value == Qt::Unchecked)
			{
				emit dataChanged(index, index, QVector<int>() << Qt::CheckStateRole);
				emit checkChanged(mapToSource(index), false);
				return true;
			}
		}
		return QIdentityProxyModel::setData(index, value, role);
	}
	QVariant data(const QModelIndex &proxyIndex, int role) const
	{
		if (proxyIndex.isValid() && role == Qt::CheckStateRole)
		{
			auto uid = proxyIndex.data(QuickModModel::UidRole).value<QuickModRef>();
			return m_instance->installedPackages()->isQuickmodWanted(uid) ? Qt::Checked
																		  : Qt::Unchecked;
		}
		return QIdentityProxyModel::data(proxyIndex, role);
	}

private slots:
	void update()
	{
		emit dataChanged(index(0, 0), index(rowCount(), columnCount()),
						 QVector<int>() << Qt::CheckStateRole);
	}

signals:
	void checkChanged(const QModelIndex &sourceIndex, const bool checked);

private:
	std::shared_ptr<OneSixInstance> m_instance;

	using QIdentityProxyModel::setSourceModel; // hide
};

QuickModBrowsePage::QuickModBrowsePage(std::shared_ptr<OneSixInstance> instance,
									   QWidget *parent)
	: QWidget(parent), ui(new Ui::QuickModBrowsePage), m_currentMod(0), m_instance(instance),
	  m_view(new QListView(this)), m_filterModel(new ModFilterProxyModel(this)),
	  m_checkModel(new CheckboxProxyModel(this))
{
	ui->setupUi(this);

	ui->viewLayout->addWidget(m_view);

	ui->tagsEdit->setValidator(new TagsValidator(this));

	m_view->setSelectionBehavior(QListView::SelectRows);
	m_view->setSelectionMode(QListView::SingleSelection);

	auto model = new QuickModModel();
	m_filterModel->setSourceModel(model);

	m_view->setModel(m_checkModel);
	ui->createInstanceButton->hide();
	m_checkModel->setSourceModel(m_instance, m_filterModel);
	connect(m_checkModel, &CheckboxProxyModel::checkChanged, this,
			&QuickModBrowsePage::checkStateChanged);

	connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
			&QuickModBrowsePage::modSelectionChanged);

	// FIXME: remove these things in the actual UI file.
	ui->mcVersionBox->clear();
	ui->mcVersionBox->addItem(m_instance->intendedVersionId());
	ui->mcVersionBox->setCurrentIndex(0);

	ui->searchLayout->removeWidget(ui->mcVersionBox);
	ui->searchLayout->removeWidget(ui->mcVersionLabel);
	ui->mcVersionBox->setVisible(false);
	ui->mcVersionLabel->setVisible(false);

#if QT_VERSION >= QT_VERSION_CHECK(5, 2, 0)
	ui->modInfoArea->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
#endif
}

QuickModBrowsePage::~QuickModBrowsePage()
{
	delete ui;
}

void QuickModBrowsePage::modLogoUpdated()
{
	ui->logoLabel->setPixmap(m_currentMod->logo());
}

void QuickModBrowsePage::checkStateChanged(const QModelIndex &index, const bool checked)
{
	try
	{
		if (checked)
		{
			auto ref = index.data(QuickModModel::UidRole).value<QuickModRef>();
			VersionSelectDialog dialog(
				new QuickModVersionModel(ref, m_instance->intendedVersionId(), this),
				tr("Choose QuickMod version for %1").arg(ref.userFacing()), this);

			if (!dialog.exec())
				return;
			std::shared_ptr<BaseQuickModVersion> version =
				std::dynamic_pointer_cast<BaseQuickModVersion>(dialog.selectedVersion());
			if (!version)
				return;
			auto transaction = m_instance->installedPackages()->getTransaction();

			transaction->setComponentVersion(ref.toString(), version->descriptor(),
											 version->mod->repo());
		}
		else
		{
			auto transaction = m_instance->installedPackages()->getTransaction();
			auto uid = index.data(QuickModModel::UidRole).value<QuickModRef>().toString();
			transaction->removeComponent(uid);
		}
	}
	catch (MMCError &e)
	{
		QMessageBox::critical(this, tr("Error"), e.cause());
	}
}

bool QuickModBrowsePage::apply()
{
	return true;
}

bool QuickModBrowsePage::shouldDisplay() const
{
	if (m_instance && m_instance->isRunning())
	{
		return false;
	}
	return true;
}

void QuickModBrowsePage::opened()
{
}

void QuickModBrowsePage::on_createInstanceButton_clicked()
{
	const QModelIndex index = m_view->currentIndex();
	if (!index.isValid())
	{
		return;
	}

	NewInstanceDialog dialog(this);
	dialog.setFromQuickMod(index.data(QuickModModel::UidRole).value<QuickModRef>());
	if (dialog.exec() == QDialog::Accepted)
	{

		InstancePtr newInstance;
		try
		{
			newInstance = InstanceFactory::get().addInstance(
				dialog.instName(), dialog.iconKey(), dialog.selectedVersion(),
				dialog.fromQuickMod());
		}
		catch (MMCError &error)
		{
			CustomMessageBox::selectable(this, tr("Error"), error.cause(), QMessageBox::Warning)
				->show();
			return;
		}
		findParent<QDialog>(this)->accept();
	}
}

void QuickModBrowsePage::on_addButton_clicked()
{
	QuickModAddFileDialog::run(this);
}

void QuickModBrowsePage::on_updateButton_clicked()
{
	MMC->qmdb()->updateFiles();
}

void QuickModBrowsePage::on_createFromInstanceBtn_clicked()
{
	QuickModCreateFromInstanceDialog(m_instance, this).exec();
}

void QuickModBrowsePage::on_categoriesLabel_linkActivated(const QString &link)
{
	ui->categoryBox->setCurrentText(link);
	ui->tagsEdit->setText(QString());
}

void QuickModBrowsePage::on_tagsLabel_linkActivated(const QString &link)
{
	ui->tagsEdit->setText(ui->tagsEdit->text() + ", " + link);
	ui->categoryBox->setCurrentText(QString());
	on_tagsEdit_textChanged();
}

void QuickModBrowsePage::on_mcVersionsLabel_linkActivated(const QString &link)
{
	ui->mcVersionBox->setCurrentText(link);
}

void QuickModBrowsePage::on_fulltextEdit_textChanged()
{
	m_filterModel->setFulltext(ui->fulltextEdit->text());
}

void QuickModBrowsePage::on_tagsEdit_textChanged()
{
	m_filterModel->setTags(
		ui->tagsEdit->text().split(QRegularExpression(", {0,1}"), QString::SkipEmptyParts));
}

void QuickModBrowsePage::on_categoryBox_currentTextChanged()
{
	m_filterModel->setCategory(ui->categoryBox->currentText());
}

void QuickModBrowsePage::on_mcVersionBox_currentTextChanged()
{
	m_filterModel->setMCVersion(ui->mcVersionBox->currentText());
}

void QuickModBrowsePage::modSelectionChanged(const QItemSelection &selected,
											 const QItemSelection &deselected)
{
	if (m_currentMod)
	{
		disconnect(m_currentMod->imagesLoader(), &QuickModImagesLoader::logoUpdated, this,
				   &QuickModBrowsePage::modLogoUpdated);
	}

	if (selected.isEmpty())
	{
		m_currentMod = 0;
		ui->nameLabel->setText("");
		ui->descriptionLabel->setText("");
		ui->websiteLabel->setText("");
		ui->categoriesLabel->setText("");
		ui->mcVersionsLabel->setText("");
		ui->tagsLabel->setText("");
		ui->logoLabel->setPixmap(QPixmap());
	}
	else
	{
		m_currentMod = m_filterModel->index(selected.first().top(), 0)
						   .data(QuickModModel::QuickModRole)
						   .value<QuickModMetadataPtr>();
		ui->nameLabel->setText(m_currentMod->name());
		ui->descriptionLabel->setText(m_currentMod->description());
		ui->websiteLabel->setText(QString("<a href=\"%1\">%2</a>").arg(
			m_currentMod->websiteUrl().toString(QUrl::FullyEncoded),
			m_currentMod->websiteUrl().toString(QUrl::PrettyDecoded)));
		QStringList categories;
		for (const QString &category : m_currentMod->categories())
		{
			categories.append(QString("<a href=\"%1\">%1</a>").arg(category));
		}
		ui->categoriesLabel->setText(categories.join(", "));
		QStringList tags;
		for (const QString &tag : m_currentMod->tags())
		{
			tags.append(QString("<a href=\"%1\">%1</a>").arg(tag));
		}
		ui->tagsLabel->setText(tags.join(", "));
		QStringList mcVersions;
		for (const QString &mcv : MMC->qmdb()->minecraftVersions(m_currentMod->uid()))
		{
			mcVersions.append(QString("<a href=\"%1\">%1</a>").arg(mcv));
		}
		ui->mcVersionsLabel->setText(mcVersions.join(", "));
		ui->logoLabel->setPixmap(m_currentMod->logo());

		connect(m_currentMod->imagesLoader(), &QuickModImagesLoader::logoUpdated, this,
				&QuickModBrowsePage::modLogoUpdated);
	}
}

#include "QuickModBrowsePage.moc"
