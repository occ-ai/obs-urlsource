#include "plugin-support.h"
#include "outputmapping.h"
#include "ui_outputmapping.h"
#include "obs-ui-utils.h"

#include <QComboBox>
#include <QHeaderView>
#include <QStandardItem>

namespace {

const std::string default_css_props = R"(background-color: transparent;
color: #FFFFFF;
font-size: 48px;
)";
const std::string default_template_string = R"({{output}})";
} // namespace

OutputMapping::OutputMapping(const output_mapping_data &mapping_data_in,
			     update_handler_t update_handler_in, QWidget *parent)
	: QDialog(parent),
	  ui(new Ui::OutputMapping),
	  mapping_data(mapping_data_in),
	  update_handler(update_handler_in)
{
	ui->setupUi(this);

	model.setHorizontalHeaderLabels(QStringList() << "Mapping Name"
						      << "Output");
	ui->tableView->setModel(&model);
	ui->tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

	// if an item is selected in the tableView, enable the remove button
	connect(ui->tableView->selectionModel(), &QItemSelectionModel::selectionChanged,
		[this](const QItemSelection &selected) {
			const bool enable = !selected.indexes().isEmpty();
			ui->toolButton_removeMapping->setEnabled(enable);
			ui->plainTextEdit_template->setEnabled(enable);
			ui->plainTextEdit_cssProps->setEnabled(enable);

			if (enable) {
				// get the selected row
				const auto row = selected.indexes().first().row();
				// get the data and output_source of the selected row
				const auto data = model.item(row, 0)->text();
				const auto output_source = model.item(row, 1)->text();
				ui->plainTextEdit_template->blockSignals(true);
				ui->plainTextEdit_cssProps->blockSignals(true);
				ui->checkBox_unhide_Source->blockSignals(true);
				// set the plainTextEdit_template and plainTextEdit_cssProps to the template_string and css_props of the selected row
				ui->plainTextEdit_template->setPlainText(
					this->mapping_data.mappings[row].template_string.c_str());
				ui->plainTextEdit_cssProps->setPlainText(
					this->mapping_data.mappings[row].css_props.c_str());
				ui->checkBox_unhide_Source->setChecked(
					this->mapping_data.mappings[row].unhide_output_source);
				ui->plainTextEdit_template->blockSignals(false);
				ui->plainTextEdit_cssProps->blockSignals(false);
				ui->checkBox_unhide_Source->blockSignals(false);
			}
		});

	// connect toolButton_addMapping to addMapping
	connect(ui->toolButton_addMapping, &QToolButton::clicked, this, &OutputMapping::addMapping);
	// connect toolButton_removeMapping to removeMapping
	connect(ui->toolButton_removeMapping, &QToolButton::clicked, this,
		&OutputMapping::removeMapping);

	// connect plainTextEdit_template textChanged to update the mapping template_string
	connect(ui->plainTextEdit_template, &QPlainTextEdit::textChanged, [this]() {
		// get the selected row
		const auto row = ui->tableView->currentIndex().row();
		// set the template_string of the selected row to the plainTextEdit_template text
		this->mapping_data.mappings[row].template_string =
			ui->plainTextEdit_template->toPlainText().toStdString();
		// call update_handler
		this->update_handler(this->mapping_data);
	});
	connect(ui->plainTextEdit_cssProps, &QPlainTextEdit::textChanged, [this]() {
		// get the selected row
		const auto row = ui->tableView->currentIndex().row();
		// set the css_props of the selected row to the plainTextEdit_cssProps text
		this->mapping_data.mappings[row].css_props =
			ui->plainTextEdit_cssProps->toPlainText().toStdString();
		// call update_handler
		this->update_handler(this->mapping_data);
	});

	// populate the model with the mapping data
	for (const auto &mapping : mapping_data.mappings) {
		model.appendRow(QList<QStandardItem *>() << new QStandardItem(mapping.name.c_str())
							 << new QStandardItem(""));
		QComboBox *comboBox = createSourcesComboBox();
		if (!mapping.output_source.empty()) {
			// find the index of the output_source in the comboBox, use EndsWith to match the prefix
			const auto index = comboBox->findText(mapping.output_source.c_str(),
							      Qt::MatchEndsWith);
			if (index == -1) {
				obs_log(LOG_WARNING, "Output source '%s' not found in combo box",
					mapping.output_source.c_str());
				continue;
			}

			// select the output_source of the mapping in the comboBox
			comboBox->blockSignals(true);
			comboBox->setCurrentIndex(index);
			comboBox->blockSignals(false);
		}
		// add a row to the model with the data and output_source of the mapping
		// set comboBox as the index widget of the last item in the model
		ui->tableView->setIndexWidget(model.index(model.rowCount() - 1, 1), comboBox);
	}

	// connect item edit on tableView
	connect(&model, &QStandardItemModel::itemChanged, [this](QStandardItem *item) {
		// update mapping name
		if (item->column() == 0) {
			this->mapping_data.mappings[item->row()].name = item->text().toStdString();
		}
	});
}

OutputMapping::~OutputMapping()
{
	delete ui;
}

/// @brief a helper function that creates a comboBox and adds all text and media sources to the list.
///     The sources are added with a prefix to indicate the source type (Text, Image, or Media).
/// @return a pointer to the comboBox
QComboBox *OutputMapping::createSourcesComboBox()
{
	QComboBox *comboBox = new QComboBox(this);
	// add "Internal Renderer" to the comboBox
	comboBox->addItem(QString::fromStdString(none_internal_rendering));
	// add all text and media sources to the comboBox
	obs_enum_sources(add_sources_to_combobox, comboBox);
	// connect comboBox to update_handler
	connect(comboBox, &QComboBox::currentIndexChanged, [this, comboBox]() {
		// get the selected row
		const auto row = ui->tableView->currentIndex().row();
		// get the output_name of the selected item in the comboBox
		const auto output_name = comboBox->currentText().toStdString();
		// remove the prefix from the output_name if it exists
		std::string output_name_without_prefix = output_name;
		if (output_name.find("(Text) ") == 0) {
			output_name_without_prefix = output_name.substr(7);
		} else if (output_name.find("(Image) ") == 0) {
			output_name_without_prefix = output_name.substr(8);
		} else if (output_name.find("(Media) ") == 0) {
			output_name_without_prefix = output_name.substr(8);
		}
		// set the css_props of the selected row to the plainTextEdit_cssProps text
		this->mapping_data.mappings[row].output_source = output_name_without_prefix;
		// call update_handler
		this->update_handler(this->mapping_data);
	});

	return comboBox;
}

void OutputMapping::addMapping()
{
	// add row to model
	model.appendRow(QList<QStandardItem *>()
			<< new QStandardItem("Mapping") << new QStandardItem("Output"));
	QComboBox *comboBox = createSourcesComboBox();
	// set comboBox as the index widget of the last item in the model
	ui->tableView->setIndexWidget(model.index(model.rowCount() - 1, 1), comboBox);
	// add a new mapping to the mapping_data
	this->mapping_data.mappings.push_back(output_mapping{
		"Mapping", none_internal_rendering, default_template_string, default_css_props});
	// call update_handler
	this->update_handler(this->mapping_data);
}

void OutputMapping::removeMapping()
{
	// remove the mapping from the mapping_data
	this->mapping_data.mappings.erase(this->mapping_data.mappings.begin() +
					  ui->tableView->currentIndex().row());
	// remove row from model
	model.removeRow(ui->tableView->currentIndex().row());
	// call update_handler
	this->update_handler(this->mapping_data);
}
