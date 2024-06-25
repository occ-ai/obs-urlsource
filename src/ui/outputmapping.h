#ifndef OUTPUTMAPPING_H
#define OUTPUTMAPPING_H

#include <QDialog>
#include <QStandardItemModel>
#include <QComboBox>
#include <functional>

#include "mapping-data.h"

namespace Ui {
class OutputMapping;
}

typedef std::function<void(const output_mapping_data &)> update_handler_t;
class OutputMapping : public QDialog {
	Q_OBJECT

public:
	explicit OutputMapping(const output_mapping_data &mapping_data_in,
			       update_handler_t update_handler, QWidget *parent = nullptr);
	~OutputMapping();

	OutputMapping(const OutputMapping &) = delete;
	OutputMapping &operator=(const OutputMapping &) = delete;

private:
	Ui::OutputMapping *ui;
	QStandardItemModel model;
	output_mapping_data mapping_data;
	QComboBox *createSourcesComboBox();
	update_handler_t update_handler;

private slots:
	void addMapping();
	void removeMapping();
};

#endif // OUTPUTMAPPING_H
